#include "joint_solvers.h"

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

#include <cmath>

namespace engine::physics {

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

  // Hooke's law: F = -k * x
  float springForce = -stiffness * displacement;

  // Damping: F_damp = -c * v_rel_along_dir
  if ((ctx.bodyA != nullptr) && (ctx.bodyB != nullptr)) {
    const math::Vec3 relVel =
        math::sub(ctx.bodyB->velocity, ctx.bodyA->velocity);
    const float relVelAlongDir = math::dot(relVel, dir);
    springForce -= damping * relVelAlongDir;
  }

  // Convert force to positional correction: dx = F * dt^2 / m
  // This is a semi-implicit integration for position-based springs.
  const float lambda = -springForce * deltaSeconds * deltaSeconds;

  ctx.tA->position = math::add(
      ctx.tA->position, math::mul(dir, lambda * ctx.invMassA / invMassSum));
  ctx.tB->position = math::sub(
      ctx.tB->position, math::mul(dir, lambda * ctx.invMassB / invMassSum));

  // Also adjust velocities for damping to take effect next frame.
  if ((ctx.bodyA != nullptr) && (ctx.bodyB != nullptr)) {
    const float impulse = springForce * deltaSeconds;
    ctx.bodyA->velocity =
        math::sub(ctx.bodyA->velocity, math::mul(dir, impulse * ctx.invMassA));
    ctx.bodyB->velocity =
        math::add(ctx.bodyB->velocity, math::mul(dir, impulse * ctx.invMassB));
  }

  accumulatedImpulse += std::fabs(lambda);
  return std::fabs(lambda);
}

} // namespace engine::physics
