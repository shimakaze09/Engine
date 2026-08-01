// Spring joint: soft distance constraint between the body origins driven
// by Hooke's law with axial damping.
// Model: along n = (xB - xA) / |xB - xA| the force is
// F = -k (|xB - xA| - L) - c (n . (vB - vA)); the same center-line
// Jacobian as the distance joint applies (J = [-n^T, 0, n^T, 0], effective
// inverse mass mA^-1 + mB^-1), but the correction is the integrated force,
// not a projection: a positional nudge dx = F dt^2 (semi-implicit) plus a
// velocity impulse F dt split by inverse mass so damping carries into the
// next frame. No velocity projection runs - the joint is soft by
// contract, and removing radial velocity would turn it rigid. The
// accumulated impulse keeps the SIGNED correction: the warm start replays
// it along the center line, so its direction must survive.

#include "joint_solvers.h"

#include "engine/math/vec3.h"

#include <cmath>

namespace engine::physics {

float solve_spring_joint(JointSolveContext &ctx, PhysicsJointSlot &joint,
                         float deltaSeconds) noexcept {
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
  const float displacement = currentDist - joint.distance;

  float springForce = -joint.stiffness * displacement;

  if ((ctx.bodyA != nullptr) && (ctx.bodyB != nullptr)) {
    const math::Vec3 relVel =
        math::sub(ctx.bodyB->velocity, ctx.bodyA->velocity);
    const float relVelAlongDir = math::dot(relVel, dir);
    springForce -= joint.damping * relVelAlongDir;
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

  joint.accumulatedImpulse += lambda;
  return std::fabs(lambda);
}

} // namespace engine::physics
