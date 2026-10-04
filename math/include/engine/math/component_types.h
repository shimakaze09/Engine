// Declares component types types and APIs for the Engine math library.

#pragma once

// Shared component POD types used across engine modules.
// Lives in math because Transform, RigidBody, Collider depend on Vec3/Quat.

#include "engine/core/asset_identity.h"
#include "engine/core/entity.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"

#include <cstdint>

namespace engine::math {

// Re-export entity types into math namespace for convenience.
using engine::core::Entity;
using engine::core::kInvalidEntity;
using engine::core::kInvalidPersistentId;
using engine::core::PersistentId;

/// Local TRS plus optional parent persistent id (ECS component POD).
struct Transform final {
  Vec3 position = Vec3(0.0F, 0.0F, 0.0F);
  Quat rotation = Quat();
  Vec3 scale = Vec3(1.0F, 1.0F, 1.0F);
  PersistentId parentId = kInvalidPersistentId;
};

/// How the simulation moves a body, as in Jolt's motion types and Unity's
/// isKinematic. Stored in RigidBody::bodyType as its numeric value.
enum class BodyType : std::uint32_t {
  /// Moved by forces, gravity and contacts; its inverse mass is its own.
  Dynamic = 0U,
  /// Moved only by its velocity and by script writes. It collides, never
  /// yields to a contact (infinite mass), feels no gravity and never
  /// sleeps, so the bodies it meets are pushed and carried by its motion.
  Kinematic = 1U,
  /// Never moves on its own and has no velocity; it collides as immovable.
  Static = 2U,
};

/// Number of BodyType values; a stored bodyType at or past it is invalid.
inline constexpr std::uint32_t kBodyTypeCount = 3U;

/// Linear/angular velocities, inverse mass/inertia, and sleep state.
struct RigidBody final {
  Vec3 velocity = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 acceleration = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 angularVelocity = Vec3(0.0F, 0.0F, 0.0F);
  float inverseMass = 1.0F;
  /// Body-space diagonal inverse inertia tensor (1 / I about each body
  /// axis). Zero on an axis locks rotation about it.
  Vec3 inverseInertia = Vec3(1.0F, 1.0F, 1.0F);
  /// Provenance of inverseInertia. False (automatic): the World derives the
  /// tensor from the collider geometry the body owns and rewrites it
  /// whenever that geometry, its placement, the ownership or the mass
  /// changes, so the stored value is a cache. True (authored): the value
  /// is content and the World never touches it. Provenance is explicit
  /// because no numeric value can tell the two apart.
  bool inertiaAuthored = false;
  std::uint8_t sleepFrameCount = 0U;
  bool sleeping = false;
  /// How much of the world's gravity this body feels: 1 the full pull, 0
  /// none (a driven platform, a held rock), a fraction or a negative
  /// value in between or beyond. Authored; it never depends on the
  /// world's gravity value, which is what an acceleration that cancels
  /// gravity would.
  float gravityScale = 1.0F;
  /// BodyType as its numeric value (Dynamic when zero). Authored. A
  /// Kinematic or Static body keeps its authored inverseMass and
  /// inverseInertia so switching back to Dynamic restores them; the
  /// simulation reads simulated_inverse_mass and simulated_inverse_inertia.
  std::uint32_t bodyType = 0U;
};

/// The body's type; a stored value past the enumeration reads as Dynamic
/// (the World refuses one at ingress, so only a raw write can reach this).
[[nodiscard]] constexpr BodyType body_type(const RigidBody &body) noexcept {
  return (body.bodyType < kBodyTypeCount) ? static_cast<BodyType>(body.bodyType)
                                          : BodyType::Dynamic;
}

/// The inverse mass the simulation uses: the authored one for a Dynamic
/// body, zero (immovable) for Kinematic and Static bodies.
[[nodiscard]] constexpr float
simulated_inverse_mass(const RigidBody &body) noexcept {
  return (body_type(body) == BodyType::Dynamic) ? body.inverseMass : 0.0F;
}

/// True for a body a step moves: every Kinematic body, and a Dynamic one
/// with mass. Such a body integrates in world space, so it must be a
/// transform root.
[[nodiscard]] constexpr bool body_moves(const RigidBody &body) noexcept {
  return (body_type(body) == BodyType::Kinematic) ||
         (simulated_inverse_mass(body) > 0.0F);
}

/// The inverse inertia the simulation uses: the authored one for a
/// Dynamic body with mass, zero for every immovable body.
[[nodiscard]] constexpr Vec3
simulated_inverse_inertia(const RigidBody &body) noexcept {
  return (simulated_inverse_mass(body) > 0.0F) ? body.inverseInertia
                                               : Vec3(0.0F, 0.0F, 0.0F);
}

/// Inverse inertia a freshly constructed RigidBody carries, and the tensor
/// an automatic body takes while it owns no collider geometry or is
/// static: unit inertia about every axis.
[[nodiscard]] constexpr Vec3 default_inverse_inertia() noexcept {
  return Vec3(1.0F, 1.0F, 1.0F);
}

/// True when any axis can rotate (some component of the inverse inertia
/// is positive); a locked or static body answers false.
[[nodiscard]] inline bool has_rotational_dof(const Vec3 &inverseInertia) noexcept {
  return (inverseInertia.x > 0.0F) || (inverseInertia.y > 0.0F) ||
         (inverseInertia.z > 0.0F);
}

/// Enumerates collider shape values used by the engine.
enum class ColliderShape : std::uint8_t {
  AABB = 0,
  Sphere = 1,
  Capsule = 2,
  ConvexHull = 3,
  Heightfield = 4,
  /// The triangles of a mesh asset (Collider::meshRef): static geometry
  /// only, since a triangle mesh has no volume to give a body mass.
  TriMesh = 5,
};

/// Provenance of a ConvexHull collider's payload: which canonical primitive
/// builder reproduces it. Serialized with the collider so every install path
/// (scene/prefab load, world copy, editor undo) can rebuild the hull data,
/// which lives outside the component in the world-owned physics context.
enum class HullSource : std::uint8_t {
  None = 0,
  Cylinder = 1,
  Pyramid = 2,
};

/// The engine's built-in blockout primitives, as every spawn path (the
/// editor's Create menu, Lua's engine.spawn_shape) names them. What each
/// one's collider is lives in one place: runtime::primitive_collider.
enum class PrimitiveShape : std::uint8_t {
  Cube = 0,
  Sphere = 1,
  Cylinder = 2,
  Capsule = 3,
  Pyramid = 4,
  Plane = 5,
};

/// Shape + half extents + material/filter fields for collision.
struct Collider final {
  Vec3 localPosition = Vec3(0.0F, 0.0F, 0.0F);
  Quat localRotation = Quat();
  Vec3 halfExtents = Vec3(0.5F, 0.5F, 0.5F);
  float restitution = 0.3F;
  float staticFriction = 0.5F;
  float dynamicFriction = 0.3F;
  float density = 1.0F;
  /// The layer bits this collider is on (usually one). The project names
  /// the 32 bits and sets which layers collide; two colliders collide only
  /// when each one's layer is in the other's mask and that matrix lets
  /// their layers meet. Queries hit it when its layer is in their mask.
  std::uint32_t collisionLayer = 1U;
  /// The layers this collider collides with.
  std::uint32_t collisionMask = 0xFFFFFFFFU;
  ColliderShape shape = ColliderShape::AABB;
  HullSource hullSource = HullSource::None;
  /// The mesh asset a TriMesh collider collides with; nil for every other
  /// shape.
  core::AssetRef meshRef{};
  /// A trigger reports overlaps instead of colliding: no contact response,
  /// no collision event, no CCD stop, no part in its body's mass
  /// properties, and physics queries pass through it. Physics reports when
  /// a non-trigger collider begins and ends overlapping it.
  bool isTrigger = false;
};

} // namespace engine::math
