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

struct Transform final {
  Vec3 position = Vec3(0.0F, 0.0F, 0.0F);
  Quat rotation = Quat();
  Vec3 scale = Vec3(1.0F, 1.0F, 1.0F);
  PersistentId parentId = kInvalidPersistentId;
};

struct RigidBody final {
  Vec3 velocity = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 acceleration = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 angularVelocity = Vec3(0.0F, 0.0F, 0.0F);
  float inverseMass = 1.0F;
  float inverseInertia = 1.0F;
  std::uint8_t sleepFrameCount = 0U;
  bool sleeping = false;
};

enum class ColliderShape : std::uint8_t {
  AABB = 0,
  Sphere = 1,
  Capsule = 2,
  ConvexHull = 3,
  Heightfield = 4,
};

struct Collider final {
  Vec3 halfExtents = Vec3(0.5F, 0.5F, 0.5F);
  float restitution = 0.3F;
  float staticFriction = 0.5F;
  float dynamicFriction = 0.3F;
  float density = 1.0F;
  std::uint32_t collisionLayer = 1U;
  std::uint32_t collisionMask = 0xFFFFFFFFU;
  ColliderShape shape = ColliderShape::AABB;
};

enum class MovementAuthority : std::uint8_t {
  None,
  Script,
};

} // namespace engine::math
