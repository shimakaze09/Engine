// Implements the shared constraint-projection helpers behind the joint
// solvers. Position corrections follow the position-based rigid-body form:
// for a constraint row with Jacobian J the correction is
// lambda = -C / (J M^-1 J^T) applied through M^-1 J^T, where M^-1 holds the
// scalar inverse masses and the bodies' world-space inverse inertia
// tensors. Velocity projections apply the momentum-conserving impulse that
// removes exactly the requested relative-velocity component, so equality
// constraints stop re-violating under integration while free DOFs (the
// null space of each projection) keep their motion. Every 3x3 solve goes
// through the same rank-aware pseudo-inverse, so a locked axis in any
// orientation contributes nothing and takes nothing.

#include "joint_projection.h"

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/inertia.h"

#include <cmath>

namespace engine::physics {

constexpr float kJointEpsilon = 1.0e-6F;

/// Diagonalizes the symmetric 3x3 matrix `k` by cyclic Jacobi rotations:
/// `eigenvalues[i]` pairs with the unit column `eigenvectors[.][i]`. A
/// fixed sweep count keeps the work bounded and the result deterministic;
/// float precision is reached in far fewer sweeps for a 3x3.
static void eigen_symmetric3(const float k[3][3], float eigenvalues[3],
                             float eigenvectors[3][3]) noexcept {
  constexpr int kMaxSweeps = 16;
  float a[3][3] = {{k[0][0], k[0][1], k[0][2]},
                   {k[1][0], k[1][1], k[1][2]},
                   {k[2][0], k[2][1], k[2][2]}};
  float v[3][3] = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
  for (int sweep = 0; sweep < kMaxSweeps; ++sweep) {
    const float off = (a[0][1] * a[0][1]) + (a[0][2] * a[0][2]) +
                      (a[1][2] * a[1][2]);
    if (off == 0.0F) {
      break;
    }
    for (int p = 0; p < 2; ++p) {
      for (int q = p + 1; q < 3; ++q) {
        const float apq = a[p][q];
        if (apq == 0.0F) {
          continue;
        }
        const float theta = (a[q][q] - a[p][p]) / (2.0F * apq);
        const float t = ((theta >= 0.0F) ? 1.0F : -1.0F) /
                        (std::fabs(theta) + std::sqrt((theta * theta) + 1.0F));
        const float c = 1.0F / std::sqrt((t * t) + 1.0F);
        const float s = t * c;
        // Rotate rows and columns p and q of a, keeping it symmetric.
        for (int r = 0; r < 3; ++r) {
          const float arp = a[r][p];
          const float arq = a[r][q];
          a[r][p] = (c * arp) - (s * arq);
          a[r][q] = (s * arp) + (c * arq);
        }
        for (int col = 0; col < 3; ++col) {
          const float apc = a[p][col];
          const float aqc = a[q][col];
          a[p][col] = (c * apc) - (s * aqc);
          a[q][col] = (s * apc) + (c * aqc);
        }
        for (int r = 0; r < 3; ++r) {
          const float vrp = v[r][p];
          const float vrq = v[r][q];
          v[r][p] = (c * vrp) - (s * vrq);
          v[r][q] = (s * vrp) + (c * vrq);
        }
      }
    }
  }
  for (int i = 0; i < 3; ++i) {
    eigenvalues[i] = a[i][i];
    for (int r = 0; r < 3; ++r) {
      eigenvectors[r][i] = v[r][i];
    }
  }
}

/// Solves K x = b for a symmetric positive semi-definite K through its
/// pseudo-inverse: x = sum over the eigenpairs whose eigenvalue is above
/// a floor RELATIVE to the largest (so validity does not depend on
/// absolute mass units) of (v . b / w) v. The part of b outside K's range
/// is the motion the bodies cannot perform, and it is left alone rather
/// than approximated axis by axis. False only when K is zero at working
/// precision: nothing can move.
static bool solve_psd3(const float k[3][3], const math::Vec3 &b,
                       math::Vec3 *out) noexcept {
  constexpr float kRelativeEigenvalueEpsilon = 1.0e-6F;
  float w[3] = {};
  float v[3][3] = {};
  eigen_symmetric3(k, w, v);
  float wMax = 0.0F;
  for (const float eigenvalue : w) {
    if (eigenvalue > wMax) {
      wMax = eigenvalue;
    }
  }
  if (wMax <= 0.0F) {
    return false;
  }
  const float floor = kRelativeEigenvalueEpsilon * wMax;
  *out = math::Vec3(0.0F, 0.0F, 0.0F);
  for (int i = 0; i < 3; ++i) {
    if (w[i] <= floor) {
      continue;
    }
    const math::Vec3 axis(v[0][i], v[1][i], v[2][i]);
    const float coefficient = math::dot(axis, b) / w[i];
    *out = math::add(*out, math::mul(axis, coefficient));
  }
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
  if (!solve_psd3(k, remove, &impulse)) {
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
