#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include <cmath>

namespace engine::physics {

float solve_fixed_joint(JointSolveContext &ctx, const math::Vec3 &anchorA,
                        const math::Vec3 &anchorB,
                        float &accumulatedImpulse) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
  }

  const float invMassSum = ctx.invMassA + ctx.invMassB;
  if (invMassSum <= 0.0F) {
    return 0.0F;
  }

  // Position constraint: anchor points must coincide (same as ball-socket).
  const math::Vec3 worldA = math::add(ctx.tA->position, anchorA);
  const math::Vec3 worldB = math::add(ctx.tB->position, anchorB);
  const math::Vec3 error = math::sub(worldB, worldA);
  const float errorLen = math::length(error);

  float lambda = 0.0F;
  if (errorLen > 1e-6F) {
    const math::Vec3 dir = math::div(error, errorLen);
    lambda = errorLen;
    ctx.tA->position = math::add(
        ctx.tA->position, math::mul(dir, lambda * ctx.invMassA / invMassSum));
    ctx.tB->position = math::sub(
        ctx.tB->position, math::mul(dir, lambda * ctx.invMassB / invMassSum));
  }

  // Velocity constraint: zero relative velocity.
  if ((ctx.bodyA != nullptr) && (ctx.bodyB != nullptr)) {
    const math::Vec3 relVel =
        math::sub(ctx.bodyB->velocity, ctx.bodyA->velocity);
    const float relVelLen = math::length(relVel);
    if (relVelLen > 1e-6F) {
      const math::Vec3 velCorrection = math::mul(relVel, 1.0F);
      ctx.bodyA->velocity =
          math::add(ctx.bodyA->velocity,
                    math::mul(velCorrection, ctx.invMassA / invMassSum));
      ctx.bodyB->velocity =
          math::sub(ctx.bodyB->velocity,
                    math::mul(velCorrection, ctx.invMassB / invMassSum));
    }
  }

  accumulatedImpulse += lambda;
  return lambda;
}

} // namespace engine::physics
