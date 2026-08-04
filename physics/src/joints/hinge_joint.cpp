// Hinge joint: pins the anchors together and permits relative rotation
// only about one shared axis, with optional twist limits in radians.
// Point block: C = pB - pA = 0 exactly as ball_joint.cpp (see that file
// for the row Jacobian and anchor-mass derivation).
// Axis block: with world axes aA = RA a_local and aB = RB b_local the
// constraint is aB x aA = 0 (two angular DOF). For the correction axis
// h = (aB x aA) / |aB x aA| the Jacobian is J = [0, -h^T, 0, h^T] with
// effective inverse mass J M^-1 J^T = iA + iB, so the misalignment angle
// atan2(|aB x aA|, aB . aA) is consumed inertia-split each iteration. At
// exact anti-parallel alignment the cross product vanishes and cannot
// name a correction axis, so a pi rotation about body A's world twist
// reference (perpendicular to the axis by construction, deterministic)
// breaks the singularity instead of silently accepting the flip.
// Limit block: the twist angle about the common axis is measured between
// creation-time reference vectors projected into the hinge plane,
// phi = atan2((uA x uB) . a, uA . uB); the violation
// phi - clamp(phi, min, max) is an angular correction about a with the
// same Jacobian shape as the axis block. Because atan2 wraps at +/-pi,
// the clamp runs on a CONTINUOUS twist accumulated from shortest-arc
// deltas of successive measurements (per-step rotation is far below pi,
// so the shortest arc is the true delta): with limits near +/-pi the
// wrapped value alone lands on the far side of the thin forbidden arc,
// clamps against the wrong boundary, and flips the outward-rate gate's
// sign, turning the limit into a turnstile for fast spins.
// Velocity projection removes the off-axis relative angular velocity, the
// relative anchor velocity, and - only while a limit is violated - the
// outward axial rate, mirroring how contacts absorb approach velocity.

#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include <cmath>

#include "joint_projection.h"

namespace engine::physics {

constexpr float kHingeEpsilon = 1.0e-6F;
constexpr float kHingePi = 3.14159265F;
constexpr float kHingeTwoPi = 6.28318531F;

/// Wraps an angle into [-pi, pi]; inputs stay within a few turns, so the
/// bounded correction loop is exact and deterministic.
static float wrap_to_pi(float angle) noexcept {
  while (angle > kHingePi) {
    angle -= kHingeTwoPi;
  }
  while (angle < -kHingePi) {
    angle += kHingeTwoPi;
  }
  return angle;
}

/// Signed twist of B relative to A about `axis` from the projected
/// creation-time references; false when a reference degenerates.
static bool measure_twist(const math::Vec3 &axis, const math::Vec3 &refA,
                          const math::Vec3 &refB, float *outAngle) noexcept {
  const math::Vec3 planarA =
      math::sub(refA, math::mul(axis, math::dot(refA, axis)));
  const math::Vec3 planarB =
      math::sub(refB, math::mul(axis, math::dot(refB, axis)));
  if ((math::length_sq(planarA) <= kHingeEpsilon * kHingeEpsilon) ||
      (math::length_sq(planarB) <= kHingeEpsilon * kHingeEpsilon)) {
    return false;
  }
  *outAngle = std::atan2(math::dot(math::cross(planarA, planarB), axis),
                         math::dot(planarA, planarB));
  return true;
}

float solve_hinge_joint(JointSolveContext &ctx,
                        PhysicsJointSlot &joint) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
  }

  const math::Vec3 axisA = joint_world_lever(*ctx.tA, joint.axis);
  const math::Vec3 axisB = joint_world_lever(*ctx.tB, joint.axisB);
  const math::Vec3 misalign = math::cross(axisB, axisA);
  const float misalignLen = math::length(misalign);
  if (misalignLen > kHingeEpsilon) {
    const float angle = std::atan2(misalignLen, math::dot(axisB, axisA));
    apply_relative_orientation_delta(
        ctx, math::mul(math::div(misalign, misalignLen), angle));
  } else if (math::dot(axisB, axisA) < 0.0F) {
    apply_relative_orientation_delta(
        ctx, math::mul(joint_world_lever(*ctx.tA, joint.twistRefA),
                       3.14159265F));
  }

  const math::Vec3 hingeAxis = joint_world_lever(*ctx.tA, joint.axis);
  float twistExcess = 0.0F;
  if (joint.hasLimits) {
    float twist = 0.0F;
    if (measure_twist(hingeAxis, joint_world_lever(*ctx.tA, joint.twistRefA),
                      joint_world_lever(*ctx.tB, joint.twistRefB), &twist)) {
      if (!joint.twistTracked) {
        joint.twistContinuous = twist;
        joint.twistTracked = true;
      } else {
        joint.twistContinuous +=
            wrap_to_pi(twist - wrap_to_pi(joint.twistContinuous));
      }
      const float clamped =
          (joint.twistContinuous < joint.minLimit)
              ? joint.minLimit
              : ((joint.twistContinuous > joint.maxLimit)
                     ? joint.maxLimit
                     : joint.twistContinuous);
      twistExcess = joint.twistContinuous - clamped;
      if (std::fabs(twistExcess) > kHingeEpsilon) {
        apply_relative_orientation_delta(ctx,
                                         math::mul(hingeAxis, -twistExcess));
      }
    }
  }

  math::Vec3 leverA = joint_world_lever(*ctx.tA, joint.anchorA);
  math::Vec3 leverB = joint_world_lever(*ctx.tB, joint.anchorB);
  const math::Vec3 error =
      math::sub(math::add(ctx.tB->position, leverB),
                math::add(ctx.tA->position, leverA));
  const float lambda = project_point_position(ctx, leverA, leverB, error);

  leverA = joint_world_lever(*ctx.tA, joint.anchorA);
  leverB = joint_world_lever(*ctx.tB, joint.anchorB);
  project_point_velocity(ctx, leverA, leverB,
                         relative_anchor_velocity(ctx, leverA, leverB));

  math::Vec3 angularRel{};
  if (ctx.bodyB != nullptr) {
    angularRel = ctx.bodyB->angularVelocity;
  }
  if (ctx.bodyA != nullptr) {
    angularRel = math::sub(angularRel, ctx.bodyA->angularVelocity);
  }
  const math::Vec3 currentAxis = joint_world_lever(*ctx.tA, joint.axis);
  const float axialRate = math::dot(angularRel, currentAxis);
  project_relative_angular_velocity(
      ctx, math::sub(angularRel, math::mul(currentAxis, axialRate)));
  if (((twistExcess > kHingeEpsilon) && (axialRate > 0.0F)) ||
      ((twistExcess < -kHingeEpsilon) && (axialRate < 0.0F))) {
    project_relative_angular_velocity(ctx,
                                      math::mul(currentAxis, axialRate));
  }

  joint.accumulatedImpulse += lambda + std::fabs(twistExcess);
  return lambda;
}

} // namespace engine::physics
