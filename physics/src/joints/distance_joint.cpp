// Distance joint: keeps the body origins at a fixed separation.
// Model: C = |xB - xA| - L = 0 with unit axis n = (xB - xA) / |xB - xA|.
// The Jacobian is J = [-n^T, 0, n^T, 0] (the anchors are the mass centers,
// so no angular terms arise), giving effective inverse mass
// J M^-1 J^T = mA^-1 + mB^-1. Each iteration applies the full positional
// correction lambda = -C split by inverse mass, then removes the radial
// relative velocity n . (vB - vA) with a momentum-conserving impulse so
// integration cannot re-stretch the rod it just corrected; tangential
// velocity (swing) lies in the projection's null space. The accumulated
// impulse keeps the SIGNED error: the warm start replays it along the
// center line, so compression (negative) must push apart, not pull
// together.

#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include <cmath>

#include "joint_projection.h"

namespace engine::physics {

float solve_distance_joint(JointSolveContext &ctx,
                           PhysicsJointSlot &joint) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
  }

  const float invMassSum = ctx.invMassA + ctx.invMassB;
  if (invMassSum <= 0.0F) {
    return 0.0F;
  }

  const math::Vec3 delta = math::sub(ctx.tB->position, ctx.tA->position);
  const float currentDist = math::length(delta);
  if (currentDist < 1e-8F) {
    return 0.0F;
  }

  const float error = currentDist - joint.distance;
  const math::Vec3 dir = math::div(delta, currentDist);
  const math::Vec3 correction = math::mul(dir, error);

  ctx.tA->position = math::add(
      ctx.tA->position, math::mul(correction, ctx.invMassA / invMassSum));
  ctx.tB->position = math::sub(
      ctx.tB->position, math::mul(correction, ctx.invMassB / invMassSum));

  const math::Vec3 zeroLever{};
  const math::Vec3 relVel = relative_anchor_velocity(ctx, zeroLever, zeroLever);
  project_point_velocity(ctx, zeroLever, zeroLever,
                         math::mul(dir, math::dot(relVel, dir)));

  joint.accumulatedImpulse += error;
  return std::fabs(error);
}

} // namespace engine::physics
