// Implements the shared constraint-projection helpers behind the joint
// solvers. Position corrections follow the position-based rigid-body form:
// for a constraint row with Jacobian J the correction is
// lambda = -C / (J M^-1 J^T) applied through M^-1 J^T, where M^-1 holds the
// scalar inverse masses and isotropic scalar inverse inertias. Velocity
// projections apply the momentum-conserving impulse that removes exactly
// the requested relative-velocity component, so equality constraints stop
// re-violating under integration while free DOFs (the null space of each
// projection) keep their motion.

#include "joint_projection.h"

#include "engine/math/quat.h"
#include "engine/math/vec3.h"

#include <cmath>

namespace engine::physics {

constexpr float kJointEpsilon = 1.0e-6F;

/// Solves the symmetric positive-definite 3x3 system K x = b by Cholesky
/// factorization with pivots checked RELATIVE to the matrix scale (largest
/// diagonal entry), so validity does not depend on absolute mass units:
/// a matrix built from tiny inverse masses (very heavy bodies) still
/// solves, while a genuinely rank-deficient matrix is rejected at any
/// scale. Returns false when K is not positive definite at working
/// precision.
static bool solve_spd3(const float k[3][3], const math::Vec3 &b,
                       math::Vec3 *out) noexcept {
  constexpr float kRelativePivotEpsilon = 1.0e-7F;
  const float maxDiag =
      (k[0][0] > k[1][1]) ? ((k[0][0] > k[2][2]) ? k[0][0] : k[2][2])
                          : ((k[1][1] > k[2][2]) ? k[1][1] : k[2][2]);
  if (maxDiag <= 0.0F) {
    return false;
  }
  const float pivotFloor = kRelativePivotEpsilon * maxDiag;

  const float d0 = k[0][0];
  if (d0 <= pivotFloor) {
    return false;
  }
  const float l00 = std::sqrt(d0);
  const float l10 = k[0][1] / l00;
  const float l20 = k[0][2] / l00;

  const float d1 = k[1][1] - (l10 * l10);
  if (d1 <= pivotFloor) {
    return false;
  }
  const float l11 = std::sqrt(d1);
  const float l21 = (k[1][2] - (l20 * l10)) / l11;

  const float d2 = k[2][2] - (l20 * l20) - (l21 * l21);
  if (d2 <= pivotFloor) {
    return false;
  }
  const float l22 = std::sqrt(d2);

  const float y0 = b.x / l00;
  const float y1 = (b.y - (l10 * y0)) / l11;
  const float y2 = (b.z - (l20 * y0) - (l21 * y1)) / l22;

  out->z = y2 / l22;
  out->y = (y1 - (l21 * out->z)) / l11;
  out->x = (y0 - (l10 * out->y) - (l20 * out->z)) / l00;
  return true;
}

/// Adds one body's anchor mass contribution i(|r|^2 I - r r^T) + m^-1 I.
static void accumulate_anchor_mass(float k[3][3], float invMass,
                                   float invInertia,
                                   const math::Vec3 &lever) noexcept {
  const float leverSq = math::length_sq(lever);
  k[0][0] += invMass + (invInertia * (leverSq - (lever.x * lever.x)));
  k[1][1] += invMass + (invInertia * (leverSq - (lever.y * lever.y)));
  k[2][2] += invMass + (invInertia * (leverSq - (lever.z * lever.z)));
  k[0][1] -= invInertia * lever.x * lever.y;
  k[0][2] -= invInertia * lever.x * lever.z;
  k[1][2] -= invInertia * lever.y * lever.z;
}

math::Vec3 joint_world_lever(const Transform &transform,
                             const math::Vec3 &localAnchor) noexcept {
  return math::rotate_vector(localAnchor,
                             math::normalize(transform.rotation));
}

void apply_orientation_delta(Transform &transform,
                             const math::Vec3 &rotVec) noexcept {
  const float angleSq = math::length_sq(rotVec);
  if (angleSq <= kJointEpsilon * kJointEpsilon) {
    return;
  }
  const float angle = std::sqrt(angleSq);
  const math::Vec3 axis = math::div(rotVec, angle);
  transform.rotation = math::normalize(
      math::mul(math::from_axis_angle(axis, angle), transform.rotation));
}

float project_point_position(JointSolveContext &ctx, const math::Vec3 &leverA,
                             const math::Vec3 &leverB,
                             const math::Vec3 &error) noexcept {
  const float errorLen = math::length(error);
  if (errorLen <= kJointEpsilon) {
    return 0.0F;
  }

  const math::Vec3 dir = math::div(error, errorLen);
  const math::Vec3 armA = math::cross(leverA, dir);
  const math::Vec3 armB = math::cross(leverB, dir);
  const float invMassSum = ctx.invMassA + ctx.invMassB +
                           (ctx.invInertiaA * math::length_sq(armA)) +
                           (ctx.invInertiaB * math::length_sq(armB));
  if (invMassSum <= 0.0F) {
    return 0.0F;
  }

  const float lambda = errorLen / invMassSum;
  const math::Vec3 impulse = math::mul(dir, lambda);
  ctx.tA->position =
      math::add(ctx.tA->position, math::mul(impulse, ctx.invMassA));
  apply_orientation_delta(
      *ctx.tA, math::mul(math::cross(leverA, impulse), ctx.invInertiaA));
  ctx.tB->position =
      math::sub(ctx.tB->position, math::mul(impulse, ctx.invMassB));
  apply_orientation_delta(
      *ctx.tB, math::mul(math::cross(leverB, impulse), -ctx.invInertiaB));
  return lambda;
}

math::Vec3 relative_anchor_velocity(const JointSolveContext &ctx,
                                    const math::Vec3 &leverA,
                                    const math::Vec3 &leverB) noexcept {
  math::Vec3 velocityA{};
  math::Vec3 velocityB{};
  if (ctx.bodyA != nullptr) {
    velocityA = math::add(ctx.bodyA->velocity,
                          math::cross(ctx.bodyA->angularVelocity, leverA));
  }
  if (ctx.bodyB != nullptr) {
    velocityB = math::add(ctx.bodyB->velocity,
                          math::cross(ctx.bodyB->angularVelocity, leverB));
  }
  return math::sub(velocityB, velocityA);
}

float project_point_velocity(JointSolveContext &ctx, const math::Vec3 &leverA,
                             const math::Vec3 &leverB,
                             const math::Vec3 &remove) noexcept {
  if (math::length_sq(remove) <= kJointEpsilon * kJointEpsilon) {
    return 0.0F;
  }

  float k[3][3] = {{0.0F, 0.0F, 0.0F},
                   {0.0F, 0.0F, 0.0F},
                   {0.0F, 0.0F, 0.0F}};
  accumulate_anchor_mass(k, ctx.invMassA, ctx.invInertiaA, leverA);
  accumulate_anchor_mass(k, ctx.invMassB, ctx.invInertiaB, leverB);
  k[1][0] = k[0][1];
  k[2][0] = k[0][2];
  k[2][1] = k[1][2];

  math::Vec3 impulse{};
  if (!solve_spd3(k, remove, &impulse)) {
    return 0.0F;
  }

  if (ctx.bodyA != nullptr) {
    ctx.bodyA->velocity = math::add(ctx.bodyA->velocity,
                                    math::mul(impulse, ctx.invMassA));
    ctx.bodyA->angularVelocity =
        math::add(ctx.bodyA->angularVelocity,
                  math::mul(math::cross(leverA, impulse), ctx.invInertiaA));
  }
  if (ctx.bodyB != nullptr) {
    ctx.bodyB->velocity = math::sub(ctx.bodyB->velocity,
                                    math::mul(impulse, ctx.invMassB));
    ctx.bodyB->angularVelocity =
        math::sub(ctx.bodyB->angularVelocity,
                  math::mul(math::cross(leverB, impulse), ctx.invInertiaB));
  }
  return math::length(impulse);
}

float apply_relative_orientation_delta(JointSolveContext &ctx,
                                       const math::Vec3 &rotVec) noexcept {
  const float invInertiaSum = ctx.invInertiaA + ctx.invInertiaB;
  const float angleSq = math::length_sq(rotVec);
  if ((invInertiaSum <= 0.0F) ||
      (angleSq <= kJointEpsilon * kJointEpsilon)) {
    return 0.0F;
  }

  apply_orientation_delta(*ctx.tB,
                          math::mul(rotVec, ctx.invInertiaB / invInertiaSum));
  apply_orientation_delta(*ctx.tA,
                          math::mul(rotVec, -ctx.invInertiaA / invInertiaSum));
  return std::sqrt(angleSq);
}

float project_relative_angular_velocity(JointSolveContext &ctx,
                                        const math::Vec3 &remove) noexcept {
  const float invInertiaSum = ctx.invInertiaA + ctx.invInertiaB;
  if ((invInertiaSum <= 0.0F) ||
      (math::length_sq(remove) <= kJointEpsilon * kJointEpsilon)) {
    return 0.0F;
  }

  if (ctx.bodyB != nullptr) {
    ctx.bodyB->angularVelocity =
        math::sub(ctx.bodyB->angularVelocity,
                  math::mul(remove, ctx.invInertiaB / invInertiaSum));
  }
  if (ctx.bodyA != nullptr) {
    ctx.bodyA->angularVelocity =
        math::add(ctx.bodyA->angularVelocity,
                  math::mul(remove, ctx.invInertiaA / invInertiaSum));
  }
  return math::length(remove) / invInertiaSum;
}

math::Vec3 relative_orientation_correction(
    const Transform &transformA, const Transform &transformB,
    const math::Quat &reference) noexcept {
  const math::Quat target =
      math::mul(math::normalize(transformA.rotation), reference);
  math::Quat correction = math::mul(
      target, math::conjugate(math::normalize(transformB.rotation)));
  if (correction.w < 0.0F) {
    correction = math::Quat(-correction.x, -correction.y, -correction.z,
                            -correction.w);
  }

  math::Vec3 axis{};
  float angle = 0.0F;
  if (!math::to_axis_angle(correction, &axis, &angle)) {
    return math::Vec3(0.0F, 0.0F, 0.0F);
  }
  return math::mul(axis, angle);
}

} // namespace engine::physics
