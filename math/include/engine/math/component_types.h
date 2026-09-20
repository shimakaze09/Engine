// Declares component types types and APIs for the Engine math library.

#pragma once

// Shared component POD types used across engine modules.
// Lives in math because Transform, RigidBody, Collider depend on Vec3/Quat.

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
};

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

/// Shape + half extents + material/filter fields for collision.
struct Collider final {
  Vec3 localPosition = Vec3(0.0F, 0.0F, 0.0F);
  Quat localRotation = Quat();
  Vec3 halfExtents = Vec3(0.5F, 0.5F, 0.5F);
  float restitution = 0.3F;
  float staticFriction = 0.5F;
  float dynamicFriction = 0.3F;
  float density = 1.0F;
  std::uint32_t collisionLayer = 1U;
  std::uint32_t collisionMask = 0xFFFFFFFFU;
  ColliderShape shape = ColliderShape::AABB;
  HullSource hullSource = HullSource::None;
};

/// Enumerates movement authority values used by the engine.
enum class MovementAuthority : std::uint8_t {
  None,
  Script,
};

} // namespace engine::math
