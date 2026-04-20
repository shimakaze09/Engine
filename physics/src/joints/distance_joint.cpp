#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include <cmath>

namespace engine::physics {

float solve_distance_joint(JointSolveContext &ctx, float targetDistance,
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

  const float error = currentDist - targetDistance;
  const math::Vec3 dir = math::div(delta, currentDist);
  const math::Vec3 correction = math::mul(dir, error);

  ctx.tA->position = math::add(
      ctx.tA->position, math::mul(correction, ctx.invMassA / invMassSum));
  ctx.tB->position = math::sub(
      ctx.tB->position, math::mul(correction, ctx.invMassB / invMassSum));

  accumulatedImpulse += std::fabs(error);
  return std::fabs(error);
}

} // namespace engine::physics
