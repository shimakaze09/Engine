// Fixed joint: welds two bodies, locking both the anchor offset and the
// relative orientation captured at creation.
// Orientation block: with the lock q0 = qA0^-1 qB0, the error is the world
// rotation qE = (qA q0) qB^-1 still separating B from its target qA q0;
// its shortest-arc rotation vector is corrected with per-axis Jacobian
// J = [0, -e^T, 0, e^T] and effective inverse mass J M^-1 J^T = iA + iB.
// Point block: C = pB - pA = 0 exactly as ball_joint.cpp, with the shared
// anchor placed at body B's origin so the offset tracks A's rotation.
// Velocity projection removes the full relative anchor velocity and the
// full relative angular velocity: a weld leaves no free DOF, so any
// residual relative motion is constraint violation.

#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include "joint_projection.h"

namespace engine::physics {

float solve_fixed_joint(JointSolveContext &ctx,
                        PhysicsJointSlot &joint) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
  }

  apply_relative_orientation_delta(
      ctx, relative_orientation_correction(*ctx.tA, *ctx.tB,
                                           joint.referenceRotation));

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
  project_relative_angular_velocity(ctx, angularRel);

  joint.accumulatedImpulse += lambda;
  return lambda;
}

} // namespace engine::physics
