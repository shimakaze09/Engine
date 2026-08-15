// Implements contact resolution for the physics narrow phase: positional
// correction plus velocity impulses for single-point contacts, clipped
// multi-point manifolds with rotational response and per-point Coulomb
// friction, and clamped speculative contacts.

#include "contact_resolution.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
float apply_velocity_impulse(RigidBody *bodyA, RigidBody *bodyB,
                             const engine::math::Vec3 &normal, float invMassA,
                             float invMassB, float invMassSum,
                             const engine::math::Vec3 &contactOffsetA,
                             const engine::math::Vec3 &contactOffsetB,
                             float restitution, float staticFric,
                             float dynamicFric) noexcept;

// Resolve a collision between two shapes given contact normal, overlap, and
// the contact point.  Applies positional correction and velocity impulse,
// then registers a 1-point manifold cache entry (issue #123) so this pair
// warm-starts and joins relax_cached_contacts' outer iteration.
void resolve_contact(PhysicsWorldView &world,
                     const PhysicsWorldView::SimulationAccessToken &simToken,
                     Entity colliderEntityA, Entity colliderEntityB,
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
  const float appliedImpulse = apply_velocity_impulse(
      bodyA, bodyB, normal, invMassA, invMassB, invMassSum,
      engine::math::sub(contactPt, correctedCenterA),
      engine::math::sub(contactPt, correctedCenterB), combinedRest,
      combinedStaticFric, combinedDynFric);

  PhysicsContext &physicsCtx = world.physics_context();
  // Matches apply_velocity_impulse's own convention: RigidBody::
  // inverseInertia directly, zero for a static or non-rotating endpoint.
  const float invInertiaA = (bodyA != nullptr) ? bodyA->inverseInertia : 0.0F;
  const float invInertiaB = (bodyB != nullptr) ? bodyB->inverseInertia : 0.0F;
  record_single_point_contact_cache(physicsCtx, colliderEntityA,
                                    colliderEntityB, contactPt, normal,
                                    overlap, appliedImpulse, invInertiaA,
                                    invInertiaB, physicsCtx.solverFrameNumber);
}

// Writes a fresh 1-point manifold: single-point paths do not clip/match
// features, so each resolve simply replaces the prior contact (mirroring
// resolve_manifold_contact's own whole-set-replace write-back).
void record_single_point_contact_cache(
    PhysicsContext &context, Entity colliderEntityA, Entity colliderEntityB,
    const engine::math::Vec3 &contactPt, const engine::math::Vec3 &normal,
    float penetration, float accumulatedImpulse, float invInertiaA,
    float invInertiaB, std::uint32_t frameNumber) noexcept {
  ContactManifold *cached =
      manifold_acquire(context, colliderEntityA, colliderEntityB, frameNumber);
  if (cached == nullptr) {
    return;
  }
  cached->contactCount = 1U;
  cached->invInertiaA = invInertiaA;
  cached->invInertiaB = invInertiaB;
  ManifoldContact &c = cached->contacts[0];
  c.pointOnA = contactPt;
  c.pointOnB = contactPt;
  c.normal = normal;
  c.penetration = penetration;
  c.accumulatedNormalImpulse = accumulatedImpulse;
  c.featureId = 0U;
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
    // stale points fall away because the whole set is replaced. Also record
    // the box-tensor invInertia this resolve actually used (issue #123) so
    // the outer relaxation pass re-solves the same point-relative quantity
    // instead of re-deriving a possibly different value.
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
    cached->invInertiaA = invInertiaA;
    cached->invInertiaB = invInertiaB;
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

float apply_velocity_impulse(RigidBody *bodyA, RigidBody *bodyB,
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
    return impulseMagnitude;
  }
  return 0.0F;
}

// Extra outer pass over one cached manifold's points (issue #123): re-solves
// each point's normal impulse against the pair's CURRENT point-relative
// velocities (which earlier pairs in this same pass may have already
// changed), continuing to accumulate from where the primary resolve (or an
// earlier relaxation pass) left off, converging fully (kManifoldSolverIterations
// sub-passes) before moving on so this pair reaches its own local
// equilibrium given whatever the rest of this outer iteration already
// changed. Angular response (point velocity via cross(angularVelocity, r),
// using ContactManifold::invInertiaA/B -- the SAME per-endpoint value the
// originating resolve used, box-tensor for a clip vs RigidBody::
// inverseInertia directly for a single-point path, never re-derived here)
// applies only to single-point manifolds; see the invInertiaA/B comment
// below for why multi-point manifolds stay linear-only.
void relax_one_manifold(
    PhysicsWorldView &world,
    const PhysicsWorldView::SimulationAccessToken &simToken,
    ContactManifold &manifold) noexcept {
  if (manifold.contactCount == 0U) {
    return;
  }

  const Entity ownerA = world.rigid_body_owner(manifold.entityA, simToken);
  const Entity ownerB = world.rigid_body_owner(manifold.entityB, simToken);
  RigidBody *bodyA =
      (ownerA != kInvalidEntity) ? world.get_rigid_body_ptr(ownerA) : nullptr;
  RigidBody *bodyB =
      (ownerB != kInvalidEntity) ? world.get_rigid_body_ptr(ownerB) : nullptr;
  // A sleeping endpoint holds zero velocity by construction and must not be
  // woken by a relaxation pass (the primary resolve already wakes a sleeper
  // touched by a fast partner via maybe_wake_pair) -- but it is still a
  // legitimate, effectively-immovable anchor for its AWAKE neighbor, exactly
  // like a true static body. Zeroing its inverse mass here (rather than
  // skipping the whole pair) is what lets a box resting on an
  // already-asleep lower box keep converging instead of losing its extra
  // passes the moment the lower box crosses the sleep threshold.
  const float invMassA =
      ((bodyA != nullptr) && !bodyA->sleeping) ? bodyA->inverseMass : 0.0F;
  const float invMassB =
      ((bodyB != nullptr) && !bodyB->sleeping) ? bodyB->inverseMass : 0.0F;
  const float invMassSum = invMassA + invMassB;
  if (invMassSum <= 0.0F) {
    return;
  }

  PhysicsTransform transformA{};
  PhysicsTransform transformB{};
  const bool haveA =
      (bodyA != nullptr) &&
      world.get_simulation_physics_transform(ownerA, simToken, &transformA);
  const bool haveB =
      (bodyB != nullptr) &&
      world.get_simulation_physics_transform(ownerB, simToken, &transformB);
  const engine::math::Vec3 zero(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 centerA = haveA ? transformA.position : zero;
  const engine::math::Vec3 centerB = haveB ? transformB.position : zero;

  // Zeroing alongside invMassA/B above (rather than reading unconditionally)
  // keeps a sleeping endpoint immovable in both linear and angular terms.
  // A MULTI-point manifold's points additionally share both bodies'
  // rotational DOF: correcting several points' normal impulses one at a
  // time (Gauss-Seidel) without also re-solving friction after each,
  // exactly what resolve_manifold_contact's own inner loop does but this
  // pass does not, feeds a persistent one-directional spin into the shared
  // body -- reproduced as steady stack creep/drift while building this fix,
  // not mere oscillation. A lone point has no such inter-point coupling, so
  // only single-point manifolds (contactCount == 1, whether from a
  // single-point path or a clip that degenerated to one point) get the
  // angular term; multi-point manifolds stay linear-only here, matching the
  // MOST helpful and safe subset for issue #123's dominant case (flat
  // resting stacks).
  const bool singlePoint = manifold.contactCount == 1U;
  const float invInertiaA =
      ((invMassA > 0.0F) && singlePoint) ? manifold.invInertiaA : 0.0F;
  const float invInertiaB =
      ((invMassB > 0.0F) && singlePoint) ? manifold.invInertiaB : 0.0F;
  // correcting them one pass each (rather than to convergence) lets later
  // points in the SAME manifold re-perturb earlier ones every outer
  // iteration -- reproduced as visible stack creep/rotation jitter while
  // building this fix. Converging fully here, matching
  // resolve_manifold_contact's own inner-iteration depth, is what makes
  // each outer pass leave the manifold at its OWN local equilibrium before
  // the next cached pair (and, on the next outer iteration, this one again)
  // sees the update.
  for (std::size_t iteration = 0U; iteration < kManifoldSolverIterations;
       ++iteration) {
    for (std::size_t p = 0U; p < manifold.contactCount; ++p) {
      ManifoldContact &c = manifold.contacts[p];
      const engine::math::Vec3 rA = engine::math::sub(c.pointOnA, centerA);
      const engine::math::Vec3 rB = engine::math::sub(c.pointOnB, centerB);
      const engine::math::Vec3 pointVelA = engine::math::add(
          (bodyA != nullptr) ? bodyA->velocity : zero,
          (bodyA != nullptr) ? engine::math::cross(bodyA->angularVelocity, rA)
                             : zero);
      const engine::math::Vec3 pointVelB = engine::math::add(
          (bodyB != nullptr) ? bodyB->velocity : zero,
          (bodyB != nullptr) ? engine::math::cross(bodyB->angularVelocity, rB)
                             : zero);
      const float vn = engine::math::dot(
          engine::math::sub(pointVelB, pointVelA), c.normal);
      // A separating point (vn >= 0) already left contact this step -- most
      // often a legitimate restitution bounce the primary resolve just
      // applied. Relaxing it toward the target=0 rest state here would
      // erode that bounce every extra pass. Only points still approaching
      // (vn < 0, the residual-stack case this pass exists for) get
      // corrected.
      if (vn >= 0.0F) {
        continue;
      }
      const float effectiveMass =
          invMassSum +
          invInertiaA *
              engine::math::length_sq(engine::math::cross(rA, c.normal)) +
          invInertiaB *
              engine::math::length_sq(engine::math::cross(rB, c.normal));
      if (effectiveMass <= 0.0F) {
        continue;
      }
      const float rawImpulse = -vn / effectiveMass;
      const float newAccumulated =
          std::fmax(0.0F, c.accumulatedNormalImpulse + rawImpulse);
      const float impulse = newAccumulated - c.accumulatedNormalImpulse;
      c.accumulatedNormalImpulse = newAccumulated;
      if (impulse == 0.0F) {
        continue;
      }
      const engine::math::Vec3 impulseVec = engine::math::mul(c.normal, impulse);
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
  clamp_angular_speed((invMassA > 0.0F) ? bodyA : nullptr);
  clamp_angular_speed((invMassB > 0.0F) ? bodyB : nullptr);
}

// Safety clamp mirroring solve_constraints' kMaxSolverIterations pattern: a
// misconfigured cvar cannot hang the frame.
constexpr int kMaxContactRelaxationIterations = 16;

void relax_cached_contacts(
    PhysicsWorldView &world,
    const PhysicsWorldView::SimulationAccessToken &simToken,
    PhysicsContext &physicsCtx) noexcept {
  PhysicsShapeStore *store = physicsCtx.shapeStore.get();
  if (store == nullptr) {
    return;
  }
  const int configured = physicsCtx.contactRelaxationIterationsCvar;
  const int extraIterations =
      (configured > 0) ? std::min(configured, kMaxContactRelaxationIterations)
                       : 0;
  if (extraIterations <= 0) {
    return;
  }

  const std::uint32_t frameNumber = physicsCtx.solverFrameNumber;
  for (int iteration = 0; iteration < extraIterations; ++iteration) {
    for (std::size_t i = 0U; i < store->contactManifoldCount; ++i) {
      ContactManifold &manifold = store->contactManifolds[i];
      if (manifold.lastFrameUsed != frameNumber) {
        // Not touched by this frame's primary resolve: a leftover pair
        // pending eviction, not a live contact to relax.
        continue;
      }
      relax_one_manifold(world, simToken, manifold);
    }
  }
}

} // namespace engine::physics
