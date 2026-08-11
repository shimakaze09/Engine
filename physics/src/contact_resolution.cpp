// Implements contact resolution for the physics narrow phase: positional
// correction plus velocity impulses for single-point contacts, clipped
// multi-point manifolds with rotational response and per-point Coulomb
// friction, and clamped speculative contacts.

#include "contact_resolution.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "engine/math/vec3.h"
#include "engine/physics/constraint_solver.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"
#include "contact_clip.h"
#include "physics_internal.h"

namespace engine::physics {

void maybe_wake_pair(RigidBody *bodyA, RigidBody *bodyB, float vA2,
                     float vB2) noexcept {
  if ((bodyA != nullptr) && bodyA->sleeping && (vB2 > kSleepThreshold)) {
    bodyA->sleeping = false;
    bodyA->sleepFrameCount = 0U;
  }
  if ((bodyB != nullptr) && bodyB->sleeping && (vA2 > kSleepThreshold)) {
    bodyB->sleeping = false;
    bodyB->sleepFrameCount = 0U;
  }
}

// Forward declaration.
// Applies the contact impulse with friction. Restitution only acts above a
// 1 m/s approach speed: slow pushing/resting contacts absorb fully, or
// driven bodies would pump bounce energy every step and ratchet airborne.
// Every dynamic endpoint with rotational inertia carries its angular
// Jacobian row — including against static geometry (issue #111) — so the
// normal row is the full J = [-n, -(rA x n), n, (rB x n)] and friction
// applies torque through the contact lever arm, letting off-center static
// impacts rotate the body and floor friction roll a sliding sphere.
void apply_velocity_impulse(RigidBody *bodyA, RigidBody *bodyB,
                            const engine::math::Vec3 &normal, float invMassA,
                            float invMassB, float invMassSum,
                            const engine::math::Vec3 &contactOffsetA,
                            const engine::math::Vec3 &contactOffsetB,
                            float restitution, float staticFric,
                            float dynamicFric) noexcept;

// Resolve a collision between two shapes given contact normal, overlap, and
// the contact point.  Applies positional correction and velocity impulse.
void resolve_contact(PhysicsWorldView &world,
                     const PhysicsWorldView::SimulationAccessToken &simToken,
                     Entity bodyEntityA, Entity bodyEntityB,
                     const engine::math::Vec3 &bodyCenterA,
                     const engine::math::Vec3 &bodyCenterB, RigidBody *bodyA,
                     RigidBody *bodyB, float invMassA, float invMassB,
                     float invMassSum, const engine::math::Vec3 &normal,
                     float overlap, const engine::math::Vec3 &contactPt,
                     const Collider &colliderA,
                     const Collider &colliderB) noexcept {
  const float moveA = overlap * (invMassA / invMassSum);
  const float moveB = overlap * (invMassB / invMassSum);

  Transform *mutableA =
      (invMassA > 0.0F) ? world.get_transform_write_ptr(bodyEntityA, simToken)
                        : nullptr;
  Transform *mutableB =
      (invMassB > 0.0F) ? world.get_transform_write_ptr(bodyEntityB, simToken)
                        : nullptr;
  if (((invMassA > 0.0F) && (mutableA == nullptr)) ||
      ((invMassB > 0.0F) && (mutableB == nullptr))) {
    return;
  }

  if (mutableA != nullptr) {
    mutableA->position =
        engine::math::sub(mutableA->position, engine::math::mul(normal, moveA));
  }
  if (mutableB != nullptr) {
    mutableB->position =
        engine::math::add(mutableB->position, engine::math::mul(normal, moveB));
  }

  const float combinedRest =
      std::max(colliderA.restitution, colliderB.restitution);
  const float combinedStaticFric =
      std::sqrt(colliderA.staticFriction * colliderB.staticFriction);
  const float combinedDynFric =
      std::sqrt(colliderA.dynamicFriction * colliderB.dynamicFriction);
  const engine::math::Vec3 correctedCenterA =
      engine::math::sub(bodyCenterA, engine::math::mul(normal, moveA));
  const engine::math::Vec3 correctedCenterB =
      engine::math::add(bodyCenterB, engine::math::mul(normal, moveB));
  apply_velocity_impulse(bodyA, bodyB, normal, invMassA, invMassB, invMassSum,
                         engine::math::sub(contactPt, correctedCenterA),
                         engine::math::sub(contactPt, correctedCenterB),
                         combinedRest, combinedStaticFric, combinedDynFric);
}

// Sequential-impulse iterations over one clipped contact manifold.
constexpr std::size_t kManifoldSolverIterations = 8U;

// Scalar inverse inertia derived from the collider's box dimensions
// (axis-averaged box tensor: a unit cube of mass 1 answers 6). The manifold
// path uses this instead of RigidBody::inverseInertia, whose 1.0 default
// makes every box right itself in slow motion; the stored field remains the
// knob for the legacy single-point paths and joints.
float box_scalar_inverse_inertia(const Collider &collider,
                                 float invMass) noexcept {
  const float hx = std::fabs(collider.halfExtents.x);
  const float hy = std::fabs(collider.halfExtents.y);
  const float hz = std::fabs(collider.halfExtents.z);
  const float extentSq = (hx * hx) + (hy * hy) + (hz * hz);
  if ((invMass <= 0.0F) || (extentSq <= 1.0e-6F)) {
    return 0.0F;
  }
  return invMass * 4.5F / extentSq;
}

// Clamps the body's angular speed to the global cap.
void clamp_angular_speed(RigidBody *body) noexcept {
  if (body == nullptr) {
    return;
  }
  const float angSpeedSq = engine::math::length_sq(body->angularVelocity);
  if (angSpeedSq > (kMaxAngularSpeed * kMaxAngularSpeed)) {
    const float angSpeed = std::sqrt(angSpeedSq);
    body->angularVelocity =
        engine::math::mul(body->angularVelocity, kMaxAngularSpeed / angSpeed);
  }
}

// Resolves a clipped multi-point contact manifold with rotational response:
// impulses use contact-point relative velocity and the angular effective
// mass, and act against static geometry too, so a tilted box receives the
// torque that lays it flat and an overhanging box tips over its support
// edge. Friction is per-point Coulomb (clamped by that point's normal
// impulse), which also brakes twist about the contact normal — a box spun on
// the ground stops instead of yawing freely. Restitution keeps the 1 m/s
// approach-speed threshold, evaluated at the deepest point before solving.
void resolve_manifold_contact(
    PhysicsWorldView &world,
    const PhysicsWorldView::SimulationAccessToken &simToken,
    Entity colliderEntityA, Entity colliderEntityB, Entity bodyEntityA,
    Entity bodyEntityB, const engine::math::Vec3 &bodyCenterA,
    const engine::math::Vec3 &bodyCenterB, RigidBody *bodyA, RigidBody *bodyB,
    float invMassA, float invMassB, float invMassSum,
    const engine::math::Vec3 &normal, const ClippedManifold &manifold,
    const Collider &colliderA, const Collider &colliderB) noexcept {
  if ((manifold.count == 0U) || (invMassSum <= 0.0F)) {
    return;
  }

  std::size_t deepestIndex = 0U;
  for (std::size_t i = 1U; i < manifold.count; ++i) {
    if (manifold.penetrations[i] > manifold.penetrations[deepestIndex]) {
      deepestIndex = i;
    }
  }
  const float overlap = manifold.penetrations[deepestIndex];

  const float moveA = overlap * (invMassA / invMassSum);
  const float moveB = overlap * (invMassB / invMassSum);
  Transform *mutableA =
      (invMassA > 0.0F) ? world.get_transform_write_ptr(bodyEntityA, simToken)
                        : nullptr;
  Transform *mutableB =
      (invMassB > 0.0F) ? world.get_transform_write_ptr(bodyEntityB, simToken)
                        : nullptr;
  if (((invMassA > 0.0F) && (mutableA == nullptr)) ||
      ((invMassB > 0.0F) && (mutableB == nullptr))) {
    return;
  }
  if (mutableA != nullptr) {
    mutableA->position =
        engine::math::sub(mutableA->position, engine::math::mul(normal, moveA));
  }
  if (mutableB != nullptr) {
    mutableB->position =
        engine::math::add(mutableB->position, engine::math::mul(normal, moveB));
  }

  const engine::math::Vec3 centerA =
      engine::math::sub(bodyCenterA, engine::math::mul(normal, moveA));
  const engine::math::Vec3 centerB =
      engine::math::add(bodyCenterB, engine::math::mul(normal, moveB));
  const float invInertiaA =
      ((bodyA != nullptr) && (bodyA->inverseInertia > 0.0F))
          ? box_scalar_inverse_inertia(colliderA, invMassA)
          : 0.0F;
  const float invInertiaB =
      ((bodyB != nullptr) && (bodyB->inverseInertia > 0.0F))
          ? box_scalar_inverse_inertia(colliderB, invMassB)
          : 0.0F;

  const engine::math::Vec3 zero(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 rA0 =
      engine::math::sub(manifold.points[deepestIndex], centerA);
  const engine::math::Vec3 rB0 =
      engine::math::sub(manifold.points[deepestIndex], centerB);
  const engine::math::Vec3 pointVelA0 = engine::math::add(
      (bodyA != nullptr) ? bodyA->velocity : zero,
      (bodyA != nullptr) ? engine::math::cross(bodyA->angularVelocity, rA0)
                         : zero);
  const engine::math::Vec3 pointVelB0 = engine::math::add(
      (bodyB != nullptr) ? bodyB->velocity : zero,
      (bodyB != nullptr) ? engine::math::cross(bodyB->angularVelocity, rB0)
                         : zero);
  const float approachSpeed0 =
      engine::math::dot(engine::math::sub(pointVelB0, pointVelA0), normal);
  const float combinedRest =
      std::max(colliderA.restitution, colliderB.restitution);
  const float restitutionTarget =
      (-approachSpeed0 > kRestitutionSpeedThreshold)
          ? (combinedRest * -approachSpeed0)
          : 0.0F;

  static_assert(ClippedManifold::kMaxPoints <= ContactManifold::kMaxContacts,
                "cache writeback assumes every clipped point fits");
  PhysicsContext &physicsCtx = world.physics_context();
  ContactManifold *cached = manifold_acquire(
      physicsCtx, colliderEntityA, colliderEntityB,
      physicsCtx.solverFrameNumber);

  float accumulated[ClippedManifold::kMaxPoints] = {};
  if (cached != nullptr) {
    // Warm start: replay last step's solved impulse on each matched point
    // (same proximity threshold as the cache's own contact matching, plus
    // normal agreement) so resting stacks begin near their converged state
    // instead of cold-starting every step.
    constexpr float kWarmStartMatchDistSq = 0.01F;
    for (std::size_t p = 0U; p < manifold.count; ++p) {
      float bestDistSq = kWarmStartMatchDistSq;
      std::size_t match = ContactManifold::kMaxContacts;
      for (std::size_t c = 0U; c < cached->contactCount; ++c) {
        const engine::math::Vec3 diff =
            engine::math::sub(cached->contacts[c].pointOnA,
                              manifold.points[p]);
        const float distSq = engine::math::dot(diff, diff);
        if ((distSq < bestDistSq) &&
            (engine::math::dot(cached->contacts[c].normal, normal) > 0.9F)) {
          bestDistSq = distSq;
          match = c;
        }
      }
      if (match >= ContactManifold::kMaxContacts) {
        continue;
      }
      const float warmImpulse =
          cached->contacts[match].accumulatedNormalImpulse;
      if (warmImpulse <= 0.0F) {
        continue;
      }
      accumulated[p] = warmImpulse;
      const engine::math::Vec3 impulseVec =
          engine::math::mul(normal, warmImpulse);
      const engine::math::Vec3 rA =
          engine::math::sub(manifold.points[p], centerA);
      const engine::math::Vec3 rB =
          engine::math::sub(manifold.points[p], centerB);
      if ((bodyA != nullptr) && (invMassA > 0.0F)) {
        bodyA->velocity = engine::math::sub(
            bodyA->velocity, engine::math::mul(impulseVec, invMassA));
        if (invInertiaA > 0.0F) {
          bodyA->angularVelocity = engine::math::sub(
              bodyA->angularVelocity,
              engine::math::mul(engine::math::cross(rA, impulseVec),
                                invInertiaA));
        }
      }
      if ((bodyB != nullptr) && (invMassB > 0.0F)) {
        bodyB->velocity = engine::math::add(
            bodyB->velocity, engine::math::mul(impulseVec, invMassB));
        if (invInertiaB > 0.0F) {
          bodyB->angularVelocity = engine::math::add(
              bodyB->angularVelocity,
              engine::math::mul(engine::math::cross(rB, impulseVec),
                                invInertiaB));
        }
      }
    }
  }
  for (std::size_t iteration = 0U; iteration < kManifoldSolverIterations;
       ++iteration) {
    for (std::size_t p = 0U; p < manifold.count; ++p) {
      const engine::math::Vec3 rA =
          engine::math::sub(manifold.points[p], centerA);
      const engine::math::Vec3 rB =
          engine::math::sub(manifold.points[p], centerB);
      const engine::math::Vec3 pointVelA = engine::math::add(
          (bodyA != nullptr) ? bodyA->velocity : zero,
          (bodyA != nullptr) ? engine::math::cross(bodyA->angularVelocity, rA)
                             : zero);
      const engine::math::Vec3 pointVelB = engine::math::add(
          (bodyB != nullptr) ? bodyB->velocity : zero,
          (bodyB != nullptr) ? engine::math::cross(bodyB->angularVelocity, rB)
                             : zero);
      const float vn = engine::math::dot(
          engine::math::sub(pointVelB, pointVelA), normal);
      const float target = (p == deepestIndex) ? restitutionTarget : 0.0F;
      const float effectiveMass =
          invMassSum +
          invInertiaA *
              engine::math::length_sq(engine::math::cross(rA, normal)) +
          invInertiaB *
              engine::math::length_sq(engine::math::cross(rB, normal));
      if (effectiveMass <= 0.0F) {
        continue;
      }
      const float rawImpulse = -(vn - target) / effectiveMass;
      const float newAccumulated =
          std::fmax(0.0F, accumulated[p] + rawImpulse);
      const float impulse = newAccumulated - accumulated[p];
      accumulated[p] = newAccumulated;
      if (impulse == 0.0F) {
        continue;
      }
      const engine::math::Vec3 impulseVec = engine::math::mul(normal, impulse);
      if ((bodyA != nullptr) && (invMassA > 0.0F)) {
        bodyA->velocity = engine::math::sub(
            bodyA->velocity, engine::math::mul(impulseVec, invMassA));
        if (invInertiaA > 0.0F) {
          bodyA->angularVelocity = engine::math::sub(
              bodyA->angularVelocity,
              engine::math::mul(engine::math::cross(rA, impulseVec),
                                invInertiaA));
        }
      }
      if ((bodyB != nullptr) && (invMassB > 0.0F)) {
        bodyB->velocity = engine::math::add(
            bodyB->velocity, engine::math::mul(impulseVec, invMassB));
        if (invInertiaB > 0.0F) {
          bodyB->angularVelocity = engine::math::add(
              bodyB->angularVelocity,
              engine::math::mul(engine::math::cross(rB, impulseVec),
                                invInertiaB));
        }
      }
    }
  }
  const float combinedStaticFric =
      std::sqrt(colliderA.staticFriction * colliderB.staticFriction);
  const float combinedDynFric =
      std::sqrt(colliderA.dynamicFriction * colliderB.dynamicFriction);
  for (std::size_t pass = 0U; pass < 2U; ++pass) {
    for (std::size_t p = 0U; p < manifold.count; ++p) {
      if (accumulated[p] <= 0.0F) {
        continue;
      }
      const engine::math::Vec3 rA =
          engine::math::sub(manifold.points[p], centerA);
      const engine::math::Vec3 rB =
          engine::math::sub(manifold.points[p], centerB);
      const engine::math::Vec3 pointVelA = engine::math::add(
          (bodyA != nullptr) ? bodyA->velocity : zero,
          (bodyA != nullptr) ? engine::math::cross(bodyA->angularVelocity, rA)
                             : zero);
      const engine::math::Vec3 pointVelB = engine::math::add(
          (bodyB != nullptr) ? bodyB->velocity : zero,
          (bodyB != nullptr) ? engine::math::cross(bodyB->angularVelocity, rB)
                             : zero);
      const engine::math::Vec3 relVel =
          engine::math::sub(pointVelB, pointVelA);
      const engine::math::Vec3 tangentVel = engine::math::sub(
          relVel, engine::math::mul(normal, engine::math::dot(relVel, normal)));
      const float tangentSpeedSq = engine::math::length_sq(tangentVel);
      if (tangentSpeedSq <= 1e-12F) {
        continue;
      }
      const float tangentSpeed = std::sqrt(tangentSpeedSq);
      const engine::math::Vec3 tangent =
          engine::math::div(tangentVel, tangentSpeed);
      const float effectiveMass =
          invMassSum +
          invInertiaA *
              engine::math::length_sq(engine::math::cross(rA, tangent)) +
          invInertiaB *
              engine::math::length_sq(engine::math::cross(rB, tangent));
      if (effectiveMass <= 0.0F) {
        continue;
      }
      float frictionImpulse = tangentSpeed / effectiveMass;
      if (frictionImpulse >= accumulated[p] * combinedStaticFric) {
        frictionImpulse = accumulated[p] * combinedDynFric;
      }
      const engine::math::Vec3 impulseVec =
          engine::math::mul(tangent, -frictionImpulse);
      if ((bodyA != nullptr) && (invMassA > 0.0F)) {
        bodyA->velocity = engine::math::sub(
            bodyA->velocity, engine::math::mul(impulseVec, invMassA));
        if (invInertiaA > 0.0F) {
          bodyA->angularVelocity = engine::math::sub(
              bodyA->angularVelocity,
              engine::math::mul(engine::math::cross(rA, impulseVec),
                                invInertiaA));
        }
      }
      if ((bodyB != nullptr) && (invMassB > 0.0F)) {
        bodyB->velocity = engine::math::add(
            bodyB->velocity, engine::math::mul(impulseVec, invMassB));
        if (invInertiaB > 0.0F) {
          bodyB->angularVelocity = engine::math::add(
              bodyB->angularVelocity,
              engine::math::mul(engine::math::cross(rB, impulseVec),
                                invInertiaB));
        }
      }
    }
  }
  if (cached != nullptr) {
    // Write the solved state back so the next step's solve seeds from it;
    // stale points fall away because the whole set is replaced.
    cached->contactCount = manifold.count;
    for (std::size_t p = 0U; p < manifold.count; ++p) {
      ManifoldContact &c = cached->contacts[p];
      c.pointOnA = manifold.points[p];
      c.pointOnB = manifold.points[p];
      c.normal = normal;
      c.penetration = manifold.penetrations[p];
      c.accumulatedNormalImpulse = accumulated[p];
      c.featureId = 0U;
    }
  }
  clamp_angular_speed((invMassA > 0.0F) ? bodyA : nullptr);
  clamp_angular_speed((invMassB > 0.0F) ? bodyB : nullptr);
}

// Resolve a speculative contact (E2a/E2b).
// Bodies are NOT yet overlapping but are approaching. Apply a clamped velocity
// impulse to prevent penetration in the next frame — no positional correction,
// no restitution, and the impulse is clamped to zero minimum (can only push
// apart, never pull together).
void resolve_speculative_contact(RigidBody *bodyA, RigidBody *bodyB,
                                 const engine::math::Vec3 &normal,
                                 float invMassA, float invMassB,
                                 float invMassSum, float gap,
                                 float deltaSeconds) noexcept {
  if (invMassSum <= 0.0F) {
    return;
  }
  if (deltaSeconds <= 0.0F) {
    return;
  }

  const engine::math::Vec3 velA = (bodyA != nullptr)
                                      ? bodyA->velocity
                                      : engine::math::Vec3(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 velB = (bodyB != nullptr)
                                      ? bodyB->velocity
                                      : engine::math::Vec3(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 relVel = engine::math::sub(velB, velA);
  const float relVelAlongNormal = engine::math::dot(relVel, normal);

  if (relVelAlongNormal >= 0.0F) {
    return;
  }

  const float closeVel = gap / deltaSeconds;

  const float excessVel = -relVelAlongNormal - closeVel;
  if (excessVel <= 0.0F) {
    return;
  }

  const float impulseMagnitude = excessVel / invMassSum;

  if ((bodyA != nullptr) && (invMassA > 0.0F)) {
    bodyA->velocity = engine::math::sub(
        bodyA->velocity,
        engine::math::mul(normal, impulseMagnitude * invMassA));
  }
  if ((bodyB != nullptr) && (invMassB > 0.0F)) {
    bodyB->velocity = engine::math::add(
        bodyB->velocity,
        engine::math::mul(normal, impulseMagnitude * invMassB));
  }
}

void apply_velocity_impulse(RigidBody *bodyA, RigidBody *bodyB,
                            const engine::math::Vec3 &normal, float invMassA,
                            float invMassB, float invMassSum,
                            const engine::math::Vec3 &contactOffsetA,
                            const engine::math::Vec3 &contactOffsetB,
                            float restitution, float staticFric,
                            float dynamicFric) noexcept {
  const engine::math::Vec3 zeroVec(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 velA = (bodyA != nullptr) ? bodyA->velocity : zeroVec;
  const engine::math::Vec3 velB = (bodyB != nullptr) ? bodyB->velocity : zeroVec;
  const engine::math::Vec3 angVelA =
      (bodyA != nullptr) ? bodyA->angularVelocity : zeroVec;
  const engine::math::Vec3 angVelB =
      (bodyB != nullptr) ? bodyB->angularVelocity : zeroVec;
  const float invInertiaA = (bodyA != nullptr) ? bodyA->inverseInertia : 0.0F;
  const float invInertiaB = (bodyB != nullptr) ? bodyB->inverseInertia : 0.0F;
  const bool angularA = (invMassA > 0.0F) && (invInertiaA > 0.0F);
  const bool angularB = (invMassB > 0.0F) && (invInertiaB > 0.0F);
  const engine::math::Vec3 pointVelA = engine::math::add(
      velA, angularA ? engine::math::cross(angVelA, contactOffsetA) : zeroVec);
  const engine::math::Vec3 pointVelB = engine::math::add(
      velB, angularB ? engine::math::cross(angVelB, contactOffsetB) : zeroVec);
  const float relVelAlongNormal =
      engine::math::dot(engine::math::sub(pointVelB, pointVelA), normal);
  if (relVelAlongNormal < 0.0F) {
    const float effectiveRestitution =
        (-relVelAlongNormal > kRestitutionSpeedThreshold) ? restitution : 0.0F;
    const float effectiveMass =
        invMassSum +
        (angularA ? invInertiaA * engine::math::length_sq(
                                      engine::math::cross(contactOffsetA,
                                                          normal))
                  : 0.0F) +
        (angularB ? invInertiaB * engine::math::length_sq(
                                      engine::math::cross(contactOffsetB,
                                                          normal))
                  : 0.0F);
    const float impulseMagnitude =
        -(1.0F + effectiveRestitution) * relVelAlongNormal / effectiveMass;
    const engine::math::Vec3 impulseVec =
        engine::math::mul(normal, impulseMagnitude);
    if ((bodyA != nullptr) && (invMassA > 0.0F)) {
      bodyA->velocity = engine::math::sub(
          bodyA->velocity,
          engine::math::mul(normal, impulseMagnitude * invMassA));
      if (angularA) {
        bodyA->angularVelocity = engine::math::sub(
            bodyA->angularVelocity,
            engine::math::mul(engine::math::cross(contactOffsetA, impulseVec),
                              invInertiaA));
      }
    }
    if ((bodyB != nullptr) && (invMassB > 0.0F)) {
      bodyB->velocity = engine::math::add(
          bodyB->velocity,
          engine::math::mul(normal, impulseMagnitude * invMassB));
      if (angularB) {
        bodyB->angularVelocity = engine::math::add(
            bodyB->angularVelocity,
            engine::math::mul(engine::math::cross(contactOffsetB, impulseVec),
                              invInertiaB));
      }
    }

    // Friction consumes the post-normal-impulse contact-point velocities so
    // it converges to rolling instead of braking a rolling body forever.
    const engine::math::Vec3 postVelA =
        (bodyA != nullptr) ? bodyA->velocity : zeroVec;
    const engine::math::Vec3 postVelB =
        (bodyB != nullptr) ? bodyB->velocity : zeroVec;
    const engine::math::Vec3 postAngVelA =
        (bodyA != nullptr) ? bodyA->angularVelocity : zeroVec;
    const engine::math::Vec3 postAngVelB =
        (bodyB != nullptr) ? bodyB->angularVelocity : zeroVec;
    const engine::math::Vec3 postPointVelA = engine::math::add(
        postVelA,
        angularA ? engine::math::cross(postAngVelA, contactOffsetA) : zeroVec);
    const engine::math::Vec3 postPointVelB = engine::math::add(
        postVelB,
        angularB ? engine::math::cross(postAngVelB, contactOffsetB) : zeroVec);
    const engine::math::Vec3 relPointVel =
        engine::math::sub(postPointVelB, postPointVelA);
    const engine::math::Vec3 tangentVel = engine::math::sub(
        relPointVel,
        engine::math::mul(normal, engine::math::dot(relPointVel, normal)));
    const float tangentSpeedSq = engine::math::length_sq(tangentVel);
    if (tangentSpeedSq > 1e-12F) {
      const float tangentSpeed = std::sqrt(tangentSpeedSq);
      const engine::math::Vec3 tangent =
          engine::math::div(tangentVel, tangentSpeed);
      const float frictionEffectiveMass =
          invMassSum +
          (angularA ? invInertiaA * engine::math::length_sq(
                                        engine::math::cross(contactOffsetA,
                                                            tangent))
                    : 0.0F) +
          (angularB ? invInertiaB * engine::math::length_sq(
                                        engine::math::cross(contactOffsetB,
                                                            tangent))
                    : 0.0F);
      float frictionImpulse = tangentSpeed / frictionEffectiveMass;
      if (frictionImpulse >= impulseMagnitude * staticFric) {
        frictionImpulse = impulseMagnitude * dynamicFric;
      }
      const engine::math::Vec3 frictionVec =
          engine::math::mul(tangent, -frictionImpulse);
      if ((bodyA != nullptr) && (invMassA > 0.0F)) {
        bodyA->velocity = engine::math::sub(
            bodyA->velocity, engine::math::mul(frictionVec, invMassA));
        if (angularA) {
          bodyA->angularVelocity = engine::math::sub(
              bodyA->angularVelocity,
              engine::math::mul(engine::math::cross(contactOffsetA,
                                                    frictionVec),
                                invInertiaA));
        }
      }
      if ((bodyB != nullptr) && (invMassB > 0.0F)) {
        bodyB->velocity = engine::math::add(
            bodyB->velocity, engine::math::mul(frictionVec, invMassB));
        if (angularB) {
          bodyB->angularVelocity = engine::math::add(
              bodyB->angularVelocity,
              engine::math::mul(engine::math::cross(contactOffsetB,
                                                    frictionVec),
                                invInertiaB));
        }
      }
    }
    clamp_angular_speed((angularA && (bodyA != nullptr)) ? bodyA : nullptr);
    clamp_angular_speed((angularB && (bodyB != nullptr)) ? bodyB : nullptr);
  }
}

} // namespace engine::physics
