// Slider (prismatic) joint: locks the relative orientation captured at
// creation and confines body B's anchor to the rail through body A's
// anchor along an axis fixed in A's frame, with optional travel limits in
// distance units.
// Orientation block: identical to fixed_joint.cpp via the stored lock q0.
// Rail blocks: with a = RA a_local and d = pB - pA, the perpendicular
// error e = d - a (d . a) and travel s = d . a give C_perp = e = 0 and,
// when limited, C_lim = s - clamp(s, min, max) = 0. Because the rail is
// carried by body A, the constraint force takes effect on A with lever
// rA + d (the prismatic Jacobian J = [-n^T, -((rA + d) x n)^T, n^T,
// (rB x n)^T]), so a side load on the pin torques the rail body; the
// effective inverse masses follow the point-projection form with those
// levers. Velocity projection removes the full relative angular velocity,
// the perpendicular anchor velocity, and - only while a limit is
// violated - the outward axial rate.

#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include <cmath>

#include "joint_projection.h"

namespace engine::physics {

constexpr float kSliderEpsilon = 1.0e-6F;

float solve_slider_joint(JointSolveContext &ctx,
                         PhysicsJointSlot &joint) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
  }

  apply_relative_orientation_delta(
      ctx, relative_orientation_correction(*ctx.tA, *ctx.tB,
                                           joint.referenceRotation));

  float lambda = 0.0F;
  math::Vec3 railAxis = joint_world_lever(*ctx.tA, joint.axis);
  math::Vec3 leverB = joint_world_lever(*ctx.tB, joint.anchorB);
  math::Vec3 delta =
      math::sub(math::add(ctx.tB->position, leverB),
                math::add(ctx.tA->position,
                          joint_world_lever(*ctx.tA, joint.anchorA)));
  math::Vec3 leverA = math::add(joint_world_lever(*ctx.tA, joint.anchorA),
                                delta);
  const math::Vec3 perpError =
      math::sub(delta, math::mul(railAxis, math::dot(delta, railAxis)));
  if (math::length_sq(perpError) > kSliderEpsilon * kSliderEpsilon) {
    lambda += project_point_position(ctx, leverA, leverB, perpError);
  }

  float travelExcess = 0.0F;
  if (joint.hasLimits) {
    railAxis = joint_world_lever(*ctx.tA, joint.axis);
    leverB = joint_world_lever(*ctx.tB, joint.anchorB);
    delta = math::sub(math::add(ctx.tB->position, leverB),
                      math::add(ctx.tA->position,
                                joint_world_lever(*ctx.tA, joint.anchorA)));
    leverA = math::add(joint_world_lever(*ctx.tA, joint.anchorA), delta);
    const float travel = math::dot(delta, railAxis);
    const float clamped =
        (travel < joint.minLimit)
            ? joint.minLimit
            : ((travel > joint.maxLimit) ? joint.maxLimit : travel);
    travelExcess = travel - clamped;
    if (std::fabs(travelExcess) > kSliderEpsilon) {
      lambda += project_point_position(ctx, leverA, leverB,
                                       math::mul(railAxis, travelExcess));
    }
  }

  railAxis = joint_world_lever(*ctx.tA, joint.axis);
  leverB = joint_world_lever(*ctx.tB, joint.anchorB);
  delta = math::sub(math::add(ctx.tB->position, leverB),
                    math::add(ctx.tA->position,
                              joint_world_lever(*ctx.tA, joint.anchorA)));
  leverA = math::add(joint_world_lever(*ctx.tA, joint.anchorA), delta);
  const math::Vec3 relVel = relative_anchor_velocity(ctx, leverA, leverB);
  const float axialRate = math::dot(relVel, railAxis);
  math::Vec3 removeVel =
      math::sub(relVel, math::mul(railAxis, axialRate));
  if (((travelExcess > kSliderEpsilon) && (axialRate > 0.0F)) ||
      ((travelExcess < -kSliderEpsilon) && (axialRate < 0.0F))) {
    removeVel = math::add(removeVel, math::mul(railAxis, axialRate));
  }
  project_point_velocity(ctx, leverA, leverB, removeVel);

  math::Vec3 angularRel{};
  if (ctx.bodyB != nullptr) {
    angularRel = ctx.bodyB->angularVelocity;
  }
  if (ctx.bodyA != nullptr) {
    angularRel = math::sub(angularRel, ctx.bodyA->angularVelocity);
  }
  project_relative_angular_velocity(ctx, angularRel);

  joint.accumulatedImpulse += lambda;
  return lambda;
}

} // namespace engine::physics
