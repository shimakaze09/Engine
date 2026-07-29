// Declares the narrow-phase dispatch surface: the per-pair context built by
// resolve_collisions' broadphase and the shape-pair testers it dispatches to.

#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"

namespace engine::physics {

// Narrow-phase dispatch context: everything resolve_collisions computes for
// a broadphase candidate pair before shape-specific handling.
struct PairContext final {
  PhysicsWorldView &world;
  const PhysicsWorldView::SimulationAccessToken &simToken;
  PhysicsContext &physicsCtx;
  Entity entityA;
  Entity entityB;
  Entity bodyEntityA;
  Entity bodyEntityB;
  const Collider &colliderA;
  const Collider &colliderB;
  const ColliderWorldGeometry &geometryA;
  const ColliderWorldGeometry &geometryB;
  RigidBody *bodyA;
  RigidBody *bodyB;
  float invMassA;
  float invMassB;
  float invMassSum;
  engine::math::Vec3 posA;
  engine::math::Vec3 posB;
  engine::math::Vec3 bodyCenterA;
  engine::math::Vec3 bodyCenterB;
  bool requiresAffineNarrowPhase;
  float speculativeDt;
};

/// Reports whether the collider's world-space linear transform requires the
/// affine support-mapped narrow phase instead of an axis-aligned fast path.
bool has_non_identity_linear_transform(
    const ColliderWorldGeometry &geometry) noexcept;

/// Heightfield vs any shape: routes to the affine or grid-based resolver.
void narrow_phase_heightfield(const PairContext &pair) noexcept;
/// Generic GJK/EPA path for convex shapes carrying any affine hierarchy TRS;
/// faceted pairs resolve through a clipped multi-point manifold.
void narrow_phase_convex_gjk(const PairContext &pair) noexcept;
/// Capsule vs Capsule: closest points between the two core segments.
void narrow_phase_capsule_capsule(const PairContext &pair) noexcept;
/// Capsule vs Sphere (either ordering): sphere against the capsule segment.
void narrow_phase_capsule_sphere(const PairContext &pair) noexcept;
/// Capsule vs AABB (either ordering): clamps the segment endpoints and
/// the segment point nearest the box center, keeps the tightest pair,
/// with a horizontal-offset fallback for deep axis-touching contacts.
void narrow_phase_capsule_aabb(const PairContext &pair) noexcept;
/// Sphere vs Sphere: center distance against summed radii.
void narrow_phase_sphere_sphere(const PairContext &pair) noexcept;
/// AABB vs Sphere (either ordering): closest box point against the radius.
void narrow_phase_aabb_sphere(const PairContext &pair) noexcept;
/// AABB vs AABB: minimum-overlap axis with a clipped face manifold, plus
/// speculative contacts for small approaching gaps.
void narrow_phase_aabb_aabb(const PairContext &pair) noexcept;

} // namespace engine::physics
