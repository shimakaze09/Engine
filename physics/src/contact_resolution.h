// Declares the contact-resolution entry points the narrow phase drives:
// single-point and clipped-manifold resolution, speculative contacts, and
// the shared wake/impulse helpers.

#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_world_view.h"
#include "contact_clip.h"

namespace engine::physics {

/// Contact normals throughout this surface point from A toward B.

/// Wake a sleeping body if the other body has velocity above threshold.
void maybe_wake_pair(RigidBody *bodyA, RigidBody *bodyB, float vA2,
                     float vB2) noexcept;

/// Applies the contact impulse with friction. Restitution only acts above a
/// 1 m/s approach speed: slow pushing/resting contacts absorb fully, or
/// driven bodies would pump bounce energy every step and ratchet airborne.
/// Returns the applied normal impulse magnitude (0 when separating), so
/// callers can seed the persistent manifold cache for single-point paths.
float apply_velocity_impulse(RigidBody *bodyA, RigidBody *bodyB,
                             const engine::math::Vec3 &normal, float invMassA,
                             float invMassB, float invMassSum,
                             const engine::math::Vec3 &contactOffsetA,
                             const engine::math::Vec3 &contactOffsetB,
                             float restitution, float staticFric,
                             float dynamicFric) noexcept;

/// Resolve a collision between two shapes given contact normal, overlap, and
/// the contact point: positional correction plus velocity impulse. Also
/// registers the pair as a 1-point entry in the persistent manifold cache
/// (issue #123) keyed by the COLLIDER entities (compound children can differ
/// from the owning body), so single-point contacts participate in
/// relax_cached_contacts' outer iteration alongside clipped manifolds.
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
                     const Collider &colliderB) noexcept;

/// Writes (or updates) a 1-point manifold cache entry for a single-point
/// contact path (sphere/capsule fast paths, degenerate-clip fallbacks) so it
/// warm-starts and participates in the outer relaxation pass like clipped
/// manifolds. invInertiaA/B must match apply_velocity_impulse's own
/// convention (RigidBody::inverseInertia directly, zero when static or
/// non-rotating) so relax_cached_contacts re-solves the same point-relative
/// quantity the primary resolve converged. A no-op when the heap-backed
/// shape store is unavailable.
void record_single_point_contact_cache(
    PhysicsContext &context, Entity colliderEntityA, Entity colliderEntityB,
    const engine::math::Vec3 &contactPt, const engine::math::Vec3 &normal,
    float penetration, float accumulatedImpulse, float invInertiaA,
    float invInertiaB, std::uint32_t frameNumber) noexcept;

/// Runs the configured number of extra outer passes (physics.
/// contact_relaxation_iterations, 0 disables) over every manifold the
/// primary narrow-phase resolve touched this frame, re-solving each cached
/// point's normal impulse against the pair's CURRENT velocities. Lets
/// corrections propagate through contact chains (stacks) within one step
/// instead of only across successive frames' warm starts (issue #123).
/// Iterates the O(1) manifold cache's dense array directly -- never a
/// per-pair scan.
void relax_cached_contacts(
    PhysicsWorldView &world,
    const PhysicsWorldView::SimulationAccessToken &simToken,
    PhysicsContext &physicsCtx) noexcept;

/// Resolves a clipped multi-point contact manifold with rotational response
/// including against static geometry; per-point Coulomb friction also
/// brakes twist about the contact normal. Impulses warm-start from and
/// write back to the pair's persistent manifold, keyed by the collider
/// entities (issue #110).
void resolve_manifold_contact(
    PhysicsWorldView &world,
    const PhysicsWorldView::SimulationAccessToken &simToken,
    Entity colliderEntityA, Entity colliderEntityB, Entity bodyEntityA,
    Entity bodyEntityB, const engine::math::Vec3 &bodyCenterA,
    const engine::math::Vec3 &bodyCenterB, RigidBody *bodyA, RigidBody *bodyB,
    float invMassA, float invMassB, float invMassSum,
    const engine::math::Vec3 &normal, const ClippedManifold &manifold,
    const Collider &colliderA, const Collider &colliderB) noexcept;

/// Resolve a speculative contact: bodies not yet overlapping but approaching
/// get a clamped velocity impulse (no positional correction, no restitution,
/// push-apart only) so they cannot penetrate next frame.
void resolve_speculative_contact(RigidBody *bodyA, RigidBody *bodyB,
                                 const engine::math::Vec3 &normal,
                                 float invMassA, float invMassB,
                                 float invMassSum, float gap,
                                 float deltaSeconds) noexcept;

} // namespace engine::physics
