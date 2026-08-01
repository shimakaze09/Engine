// Ball-socket joint: pins a body-local anchor point on each body together
// while leaving all three relative rotations free.
// Model: world anchors p = x + R r with R the body rotation and r the
// local anchor; constraint C = pB - pA = 0. Along a direction n the
// Jacobian row is J = [-n^T, -(rA x n)^T, n^T, (rB x n)^T], giving the
// effective inverse mass J M^-1 J^T = mA^-1 + mB^-1 + iA|rA x n|^2 +
// iB|rB x n|^2 with scalar inverse inertias i. Each iteration projects the
// current error along its own direction as an impulse at the anchors (so
// offset anchors torque their bodies), then removes the full relative
// anchor velocity through the 3x3 anchor mass matrix. Rotation about the
// pinned point lies in that projection's null space, so a pendulum keeps
// swinging while the pin never separates.

#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include "joint_projection.h"

namespace engine::physics {

float solve_ball_socket_joint(JointSolveContext &ctx,
                              PhysicsJointSlot &joint) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
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

  joint.accumulatedImpulse += lambda;
  return lambda;
}

} // namespace engine::physics
