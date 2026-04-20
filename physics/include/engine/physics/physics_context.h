#pragma once

// Physics-side types that were previously nested inside runtime::World.
// Moved here so the physics module has no compile-time dependency on runtime.

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/entity.h"
#include "engine/math/component_types.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics.h"

#ifndef ENGINE_MAX_ENTITIES
#define ENGINE_MAX_ENTITIES 65536U
#endif

namespace engine::physics {

// Re-export for use in PhysicsContext arrays.
using engine::core::Entity;
using engine::core::kInvalidEntity;

// Capacity constants (previously World::kMax*).
static constexpr std::size_t kMaxPhysicsJoints = 4096U;
static constexpr std::size_t kMaxCollisionPairs = 1024U;
static constexpr std::size_t kCollisionPairHashBuckets = 4096U;
static constexpr std::size_t kMaxColliders = ENGINE_MAX_ENTITIES;

enum class JointType : std::uint8_t {
  Distance = 0,
  Hinge = 1,
  BallSocket = 2,
  Slider = 3,
  Spring = 4,
  Fixed = 5,
};

struct PhysicsJointSlot final {
  Entity entityA = kInvalidEntity;
  Entity entityB = kInvalidEntity;
  JointType type = JointType::Distance;
  bool active = false;
  bool hasLimits = false;

  // Local-space anchor offsets on each body.
  math::Vec3 anchorA{};
  math::Vec3 anchorB{};

  // Distance / Spring rest length.
  float distance = 1.0F;

  // Hinge / Slider axis (world-space direction, normalized).
  math::Vec3 axis = math::Vec3(0.0F, 1.0F, 0.0F);

  // Angle or distance limits (hinge: radians, slider: distance).
  float minLimit = 0.0F;
  float maxLimit = 0.0F;

  // Spring parameters.
  float stiffness = 100.0F;
  float damping = 1.0F;

  // Warm starting: accumulated impulse from previous frame.
  float accumulatedImpulse = 0.0F;
};

struct PhysicsContext final {
  math::Vec3 gravity = math::Vec3(0.0F, -9.8F, 0.0F);
  std::array<PhysicsJointSlot, kMaxPhysicsJoints> joints{};
  std::size_t jointCount = 0U;

  // Packed collision pairs: [entityIndexA0, entityIndexB0, ...]
  std::array<std::uint32_t, kMaxCollisionPairs * 2U> collisionPairData{};
  std::size_t collisionPairCount = 0U;
  CollisionDispatchFn collisionDispatch = nullptr;

  // O(1) collision-pair dedupe via open addressing and generation stamps.
  std::array<std::uint64_t, kCollisionPairHashBuckets> pairHashKeys{};
  std::array<std::uint32_t, kCollisionPairHashBuckets> pairHashStamps{};
  std::uint32_t pairHashGeneration = 1U;

  // O(1) broadphase neighbor dedupe using per-collider generation stamps.
  std::array<std::uint32_t, kMaxColliders> testedStamps{};
  std::uint32_t testedGeneration = 1U;
};

} // namespace engine::physics
