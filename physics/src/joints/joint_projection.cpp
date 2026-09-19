// Implements the shared constraint-projection helpers behind the joint
// solvers. Position corrections follow the position-based rigid-body form:
// for a constraint row with Jacobian J the correction is
// lambda = -C / (J M^-1 J^T) applied through M^-1 J^T, where M^-1 holds the
// scalar inverse masses and the bodies' world-space inverse inertia
// tensors. Velocity projections apply the momentum-conserving impulse that
// removes exactly the requested relative-velocity component, so equality
// constraints stop re-violating under integration while free DOFs (the
// null space of each projection) keep their motion.

#include "joint_projection.h"

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/inertia.h"

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

/// Solves K x = b where K is positive semi-definite: the full Cholesky
/// solve when K is definite, otherwise the diagonal solve that skips the
/// locked axes, so a pair of bodies both locked about one axis still
/// receives its correction about the free ones.
static bool solve_psd3(const float k[3][3], const math::Vec3 &b,
                       math::Vec3 *out) noexcept {
  if (solve_spd3(k, b, out)) {
    return true;
  }
  constexpr float kRelativePivotEpsilon = 1.0e-7F;
  const float maxDiag =
      (k[0][0] > k[1][1]) ? ((k[0][0] > k[2][2]) ? k[0][0] : k[2][2])
                          : ((k[1][1] > k[2][2]) ? k[1][1] : k[2][2]);
  if (maxDiag <= 0.0F) {
    return false;
  }
  const float pivotFloor = kRelativePivotEpsilon * maxDiag;
  out->x = (k[0][0] > pivotFloor) ? (b.x / k[0][0]) : 0.0F;
  out->y = (k[1][1] > pivotFloor) ? (b.y / k[1][1]) : 0.0F;
  out->z = (k[2][2] > pivotFloor) ? (b.z / k[2][2]) : 0.0F;
  return true;
}

/// Applies a row-major 3x3 matrix to a vector.
static math::Vec3 mat3_mul(const float m[3][3], const math::Vec3 &v) noexcept {
  return math::Vec3((m[0][0] * v.x) + (m[0][1] * v.y) + (m[0][2] * v.z),
                    (m[1][0] * v.x) + (m[1][1] * v.y) + (m[1][2] * v.z),
                    (m[2][0] * v.x) + (m[2][1] * v.y) + (m[2][2] * v.z));
}

/// World-space inverse inertia of endpoint A or B at its current
/// orientation; zero for a locked or static endpoint.
static void endpoint_inverse_inertia(const JointSolveContext &ctx, bool sideA,
                                     float out[3][3]) noexcept {
  const math::Vec3 &inverseInertia = sideA ? ctx.invInertiaA : ctx.invInertiaB;
  const Transform *transform = sideA ? ctx.tA : ctx.tB;
  inverse_inertia_world(inverseInertia, math::normalize(transform->rotation),
                        out);
}

/// Adds one body's anchor mass contribution m^-1 I + S^T I_w^-1 S, where S
/// is the cross-product matrix of the lever (S v = lever x v).
static void accumulate_anchor_mass(float k[3][3], float invMass,
                                   const float inverseInertia[3][3],
                                   const math::Vec3 &lever) noexcept {
  const float s[3][3] = {{0.0F, -lever.z, lever.y},
                         {lever.z, 0.0F, -lever.x},
                         {-lever.y, lever.x, 0.0F}};
  // t = I_w^-1 S, then k += S^T t = -S t.
  float t[3][3] = {};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      t[row][col] = (inverseInertia[row][0] * s[0][col]) +
                    (inverseInertia[row][1] * s[1][col]) +
                    (inverseInertia[row][2] * s[2][col]);
    }
  }
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      k[row][col] -= (s[row][0] * t[0][col]) + (s[row][1] * t[1][col]) +
                     (s[row][2] * t[2][col]);
    }
    k[row][row] += invMass;
  }
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
  const math::Quat rotA = math::normalize(ctx.tA->rotation);
  const math::Quat rotB = math::normalize(ctx.tB->rotation);
  const float invMassSum =
      ctx.invMassA + ctx.invMassB +
      angular_effective_inverse_mass(ctx.invInertiaA, rotA, leverA, dir) +
      angular_effective_inverse_mass(ctx.invInertiaB, rotB, leverB, dir);
  if (invMassSum <= 0.0F) {
    return 0.0F;
  }

  const float lambda = errorLen / invMassSum;
  const math::Vec3 impulse = math::mul(dir, lambda);
  ctx.tA->position =
      math::add(ctx.tA->position, math::mul(impulse, ctx.invMassA));
  apply_orientation_delta(
      *ctx.tA, apply_inverse_inertia(ctx.invInertiaA, rotA,
                                     math::cross(leverA, impulse)));
  ctx.tB->position =
      math::sub(ctx.tB->position, math::mul(impulse, ctx.invMassB));
  apply_orientation_delta(
      *ctx.tB, math::mul(apply_inverse_inertia(ctx.invInertiaB, rotB,
                                               math::cross(leverB, impulse)),
                         -1.0F));
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

  float inertiaA[3][3] = {};
  float inertiaB[3][3] = {};
  endpoint_inverse_inertia(ctx, true, inertiaA);
  endpoint_inverse_inertia(ctx, false, inertiaB);
  float k[3][3] = {{0.0F, 0.0F, 0.0F},
                   {0.0F, 0.0F, 0.0F},
                   {0.0F, 0.0F, 0.0F}};
  accumulate_anchor_mass(k, ctx.invMassA, inertiaA, leverA);
  accumulate_anchor_mass(k, ctx.invMassB, inertiaB, leverB);

  math::Vec3 impulse{};
  if (!solve_spd3(k, remove, &impulse)) {
    return 0.0F;
  }

  if (ctx.bodyA != nullptr) {
    ctx.bodyA->velocity = math::add(ctx.bodyA->velocity,
                                    math::mul(impulse, ctx.invMassA));
    ctx.bodyA->angularVelocity =
        math::add(ctx.bodyA->angularVelocity,
                  mat3_mul(inertiaA, math::cross(leverA, impulse)));
  }
  if (ctx.bodyB != nullptr) {
    ctx.bodyB->velocity = math::sub(ctx.bodyB->velocity,
                                    math::mul(impulse, ctx.invMassB));
    ctx.bodyB->angularVelocity =
        math::sub(ctx.bodyB->angularVelocity,
                  mat3_mul(inertiaB, math::cross(leverB, impulse)));
  }
  return math::length(impulse);
}

/// Splits a relative angular quantity between the endpoints: solves
/// (I_A^-1 + I_B^-1) lambda = total and hands each side its own share
/// I^-1 lambda, so a body that cannot rotate about an axis takes none of
/// that axis and the other body takes it all. False when neither side can
/// rotate.
static bool split_by_inertia(const JointSolveContext &ctx,
                             const math::Vec3 &total, math::Vec3 *outShareA,
                             math::Vec3 *outShareB,
                             math::Vec3 *outLambda) noexcept {
  float inertiaA[3][3] = {};
  float inertiaB[3][3] = {};
  endpoint_inverse_inertia(ctx, true, inertiaA);
  endpoint_inverse_inertia(ctx, false, inertiaB);
  float sum[3][3] = {};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      sum[row][col] = inertiaA[row][col] + inertiaB[row][col];
    }
  }
  math::Vec3 lambda{};
  if (!solve_psd3(sum, total, &lambda)) {
    return false;
  }
  *outShareA = mat3_mul(inertiaA, lambda);
  *outShareB = mat3_mul(inertiaB, lambda);
  *outLambda = lambda;
  return true;
}

float apply_relative_orientation_delta(JointSolveContext &ctx,
                                       const math::Vec3 &rotVec) noexcept {
  const float angleSq = math::length_sq(rotVec);
  if (angleSq <= kJointEpsilon * kJointEpsilon) {
    return 0.0F;
  }
  math::Vec3 shareA{};
  math::Vec3 shareB{};
  math::Vec3 lambda{};
  if (!split_by_inertia(ctx, rotVec, &shareA, &shareB, &lambda)) {
    return 0.0F;
  }

  apply_orientation_delta(*ctx.tB, shareB);
  apply_orientation_delta(*ctx.tA, math::mul(shareA, -1.0F));
  return std::sqrt(angleSq);
}

float project_relative_angular_velocity(JointSolveContext &ctx,
                                        const math::Vec3 &remove) noexcept {
  if (math::length_sq(remove) <= kJointEpsilon * kJointEpsilon) {
    return 0.0F;
  }
  math::Vec3 shareA{};
  math::Vec3 shareB{};
  math::Vec3 lambda{};
  if (!split_by_inertia(ctx, remove, &shareA, &shareB, &lambda)) {
    return 0.0F;
  }

  if (ctx.bodyB != nullptr) {
    ctx.bodyB->angularVelocity =
        math::sub(ctx.bodyB->angularVelocity, shareB);
  }
  if (ctx.bodyA != nullptr) {
    ctx.bodyA->angularVelocity =
        math::add(ctx.bodyA->angularVelocity, shareA);
  }
  return math::length(lambda);
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
