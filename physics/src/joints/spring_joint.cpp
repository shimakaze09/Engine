// Implements spring joint behavior for the Engine physics system.

#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include <cmath>

namespace engine::physics {

/// Position-based spring: Hooke's force with velocity damping, converted to
/// a positional correction (dx = F·dt²/m, semi-implicit) plus a velocity
/// impulse so damping carries into the next frame. The accumulated impulse
/// keeps the SIGNED correction: the warm start replays it along the center
/// line, so its direction must survive.
float solve_spring_joint(JointSolveContext &ctx, float restLength,
                         float stiffness, float damping, float deltaSeconds,
                         float &accumulatedImpulse) noexcept {
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

  const math::Vec3 dir = math::div(delta, currentDist);
  const float displacement = currentDist - restLength;

  float springForce = -stiffness * displacement;

  if ((ctx.bodyA != nullptr) && (ctx.bodyB != nullptr)) {
    const math::Vec3 relVel =
        math::sub(ctx.bodyB->velocity, ctx.bodyA->velocity);
    const float relVelAlongDir = math::dot(relVel, dir);
    springForce -= damping * relVelAlongDir;
  }

  const float lambda = -springForce * deltaSeconds * deltaSeconds;

  ctx.tA->position = math::add(
      ctx.tA->position, math::mul(dir, lambda * ctx.invMassA / invMassSum));
  ctx.tB->position = math::sub(
      ctx.tB->position, math::mul(dir, lambda * ctx.invMassB / invMassSum));

  if ((ctx.bodyA != nullptr) && (ctx.bodyB != nullptr)) {
    const float impulse = springForce * deltaSeconds;
    ctx.bodyA->velocity =
        math::sub(ctx.bodyA->velocity, math::mul(dir, impulse * ctx.invMassA));
    ctx.bodyB->velocity =
        math::add(ctx.bodyB->velocity, math::mul(dir, impulse * ctx.invMassB));
  }

  accumulatedImpulse += lambda;
  return std::fabs(lambda);
}

} // namespace engine::physics
