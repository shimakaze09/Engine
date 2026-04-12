#include "joint_solvers.h"

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

#include <cmath>

namespace engine::physics {

float solve_hinge_joint(JointSolveContext &ctx, const math::Vec3 &anchorA,
                        const math::Vec3 &anchorB, const math::Vec3 &axis,
                        bool hasLimits, float minAngle, float maxAngle,
                        float &accumulatedImpulse) noexcept {
  if ((ctx.tA == nullptr) || (ctx.tB == nullptr)) {
    return 0.0F;
  }

  const float invMassSum = ctx.invMassA + ctx.invMassB;
  if (invMassSum <= 0.0F) {
    return 0.0F;
  }

  // Position constraint: keep anchor points together (ball-socket part).
  const math::Vec3 worldA = math::add(ctx.tA->position, anchorA);
  const math::Vec3 worldB = math::add(ctx.tB->position, anchorB);
  const math::Vec3 posError = math::sub(worldB, worldA);
  const float posErrorLen = math::length(posError);

  float lambda = 0.0F;
  if (posErrorLen > 1e-6F) {
    const math::Vec3 dir = math::div(posError, posErrorLen);
    lambda = posErrorLen;
    ctx.tA->position = math::add(ctx.tA->position,
                                  math::mul(dir, lambda * ctx.invMassA / invMassSum));
    ctx.tB->position = math::sub(ctx.tB->position,
                                  math::mul(dir, lambda * ctx.invMassB / invMassSum));
  }

  // Hinge axis constraint: project the relative displacement perpendicular
  // to the axis. Without angular velocity we constrain the linear displacement
  // to be along the hinge axis only.
  if (hasLimits) {
    const math::Vec3 relPos = math::sub(ctx.tB->position, ctx.tA->position);
    const float projOnAxis = math::dot(relPos, axis);
    const float clampedProj =
        (projOnAxis < minAngle)
            ? minAngle
            : ((projOnAxis > maxAngle) ? maxAngle : projOnAxis);
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
