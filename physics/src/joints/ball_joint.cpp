#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include <cmath>

namespace engine::physics {

float solve_ball_socket_joint(JointSolveContext &ctx, const math::Vec3 &anchorA,
                              const math::Vec3 &anchorB,
                              float &accumulatedImpulse) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
  }

  const float invMassSum = ctx.invMassA + ctx.invMassB;
  if (invMassSum <= 0.0F) {
    return 0.0F;
  }

  // World-space anchor positions.
  const math::Vec3 worldA = math::add(ctx.tA->position, anchorA);
  const math::Vec3 worldB = math::add(ctx.tB->position, anchorB);

  const math::Vec3 error = math::sub(worldB, worldA);
  const float errorLen = math::length(error);
  if (errorLen < 1e-6F) {
    return 0.0F;
  }

  const math::Vec3 dir = math::div(error, errorLen);
  const float lambda = errorLen;
  accumulatedImpulse += lambda;

  ctx.tA->position = math::add(
      ctx.tA->position, math::mul(dir, lambda * ctx.invMassA / invMassSum));
  ctx.tB->position = math::sub(
      ctx.tB->position, math::mul(dir, lambda * ctx.invMassB / invMassSum));

  return lambda;
}

} // namespace engine::physics
