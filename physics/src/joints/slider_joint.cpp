#include "joint_solvers.h"

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

#include <cmath>

namespace engine::physics {

float solve_slider_joint(JointSolveContext &ctx, const math::Vec3 &axis,
                         bool hasLimits, float minDist, float maxDist,
                         float &accumulatedImpulse) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
  }

  const float invMassSum = ctx.invMassA + ctx.invMassB;
  if (invMassSum <= 0.0F) {
    return 0.0F;
  }

  const math::Vec3 relPos = math::sub(ctx.tB->position, ctx.tA->position);

  // Remove the component perpendicular to the slider axis.
  const float projOnAxis = math::dot(relPos, axis);
  const math::Vec3 onAxis = math::mul(axis, projOnAxis);
  const math::Vec3 perpError = math::sub(relPos, onAxis);
  const float perpLen = math::length(perpError);

  float lambda = 0.0F;

  // Correct perpendicular displacement (constrain to axis).
  if (perpLen > 1e-6F) {
    const math::Vec3 perpDir = math::div(perpError, perpLen);
    ctx.tA->position =
        math::add(ctx.tA->position,
                  math::mul(perpDir, perpLen * ctx.invMassA / invMassSum));
    ctx.tB->position =
        math::sub(ctx.tB->position,
                  math::mul(perpDir, perpLen * ctx.invMassB / invMassSum));
    lambda += perpLen;
  }

  // Apply distance limits along the axis.
  if (hasLimits) {
    float clampedProj = projOnAxis;
    if (clampedProj < minDist) {
      clampedProj = minDist;
    }
    if (clampedProj > maxDist) {
      clampedProj = maxDist;
    }
    const float limitError = projOnAxis - clampedProj;
    if (std::fabs(limitError) > 1e-6F) {
      const math::Vec3 correction = math::mul(axis, limitError);
      ctx.tA->position = math::add(
          ctx.tA->position, math::mul(correction, ctx.invMassA / invMassSum));
      ctx.tB->position = math::sub(
          ctx.tB->position, math::mul(correction, ctx.invMassB / invMassSum));
      lambda += std::fabs(limitError);
    }
  }

  accumulatedImpulse += lambda;
  return lambda;
}

} // namespace engine::physics
