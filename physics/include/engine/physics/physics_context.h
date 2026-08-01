// Declares physics context types and APIs for the Engine physics system.

#pragma once

// Physics-side types that were previously nested inside runtime::World.
// Moved here so the physics module has no compile-time dependency on runtime.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "engine/core/entity.h"
#include "engine/math/aabb.h"
#include "engine/math/component_types.h"
#include "engine/math/quat.h"
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
static constexpr std::size_t kMaxConvexHulls = 256U;
static constexpr std::size_t kMaxHeightfields = 16U;

/// Enumerates joint type values used by the engine.
enum class JointType : std::uint8_t {
  Distance = 0,
  Hinge = 1,
  BallSocket = 2,
  Slider = 3,
  Spring = 4,
  Fixed = 5,
};

/// One joint's type, bodies, constraint frame, parameters, and impulse.
struct PhysicsJointSlot final {
  Entity entityA = kInvalidEntity;
  Entity entityB = kInvalidEntity;
  std::uint32_t generation = 1U;
  JointType type = JointType::Distance;
  bool active = false;
  bool hasLimits = false;

  // Body-local anchor offsets, rotated by each body's current orientation
  // when solved so anchors track rotating bodies.
  math::Vec3 anchorA{};
  math::Vec3 anchorB{};

  // Distance / Spring rest length.
  float distance = 1.0F;

  // Hinge / Slider axis in body A's local frame (normalized); axisB is the
  // hinge axis expressed in body B's local frame for the alignment block.
  math::Vec3 axis = math::Vec3(0.0F, 1.0F, 0.0F);
  math::Vec3 axisB = math::Vec3(0.0F, 1.0F, 0.0F);

  // Hinge twist references: one shared creation-time vector perpendicular
  // to the axis, stored per body in that body's local frame.
  math::Vec3 twistRefA{};
  math::Vec3 twistRefB{};

  // Fixed / Slider lock: body B's orientation relative to A at creation.
  math::Quat referenceRotation{};

  // Angle or distance limits (hinge: radians, slider: distance).
  float minLimit = 0.0F;
  float maxLimit = 0.0F;

  // Spring parameters.
  float stiffness = 100.0F;
  float damping = 1.0F;

  // Distance / Spring warm-start impulse (signed, replayed along the
  // center line); other types accumulate their step's correction
  // magnitudes here as a diagnostic only.
  float accumulatedImpulse = 0.0F;
};

/// Stores large shape payloads owned by a physics context.
struct PhysicsShapeStore final {
  std::array<ConvexHullData, kMaxConvexHulls> convexHullData{};
  std::array<Entity, kMaxConvexHulls> convexHullEntity{};
  std::size_t convexHullCount = 0U;

  std::array<HeightfieldData, kMaxHeightfields> heightfieldData{};
  std::array<Entity, kMaxHeightfields> heightfieldEntity{};
  std::size_t heightfieldCount = 0U;

  // Per-collider snapshot rebuilt by resolve_collisions each step; the next
  // step's CCD reads it for cheap candidate rejection and ownership lookup
  // instead of re-walking the hierarchy per body×collider combination.
  std::array<Entity, kMaxColliders> ccdColliderEntities{};
  std::array<Entity, kMaxColliders> ccdColliderOwners{};
  std::array<math::AABB, kMaxColliders> ccdColliderAabbs{};
  // Owner body velocities captured with the snapshot: CCD's candidate
  // rejection must not read live RigidBody::velocity, which parallel chunk
  // jobs are integrating concurrently.
  std::array<math::Vec3, kMaxColliders> ccdColliderVelocities{};

  // Blocked-body warning diagnostic (physics.blocked_warn_steps): commanded
  // speeds captured at resolve entry keyed by dense rigid-body index
  // (negative = ineligible this step), and consecutive-blocked-step episode
  // counters keyed by entity index. A counter can survive an entity-index
  // reuse, which at worst fires one warning early — acceptable for a
  // log-only diagnostic that never feeds back into simulation.
  std::array<float, kMaxColliders> blockedCommandedSpeeds{};
  std::array<std::uint8_t, ENGINE_MAX_ENTITIES + 1U> blockedStepCounts{};
  std::uint32_t blockedWarningCount = 0U;
  std::uint32_t blockedLastEntityIndex = 0U;
  std::uint32_t blockedLastBlockerIndex = 0U;
};

/// World-owned physics storage: gravity, joints, pair/stamp scratch,
/// and hull/heightfield payloads.
struct PhysicsContext final {
  PhysicsContext() noexcept;
  /// Copies context data and deep-copies owned shape payloads.
  PhysicsContext(const PhysicsContext &other) noexcept;
  /// Copies context data and deep-copies owned shape payloads.
  PhysicsContext &operator=(const PhysicsContext &other) noexcept;
  PhysicsContext(PhysicsContext &&other) noexcept = default;
  PhysicsContext &operator=(PhysicsContext &&other) noexcept = default;
  ~PhysicsContext() = default;

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

  // Valid entry count and compound flag for the shape store's CCD snapshot
  // (one step stale after reparenting, an Input-phase-only mutation). When
  // no compound colliders exist, CCD sweeps each body's own collider.
  std::size_t ccdColliderCount = 0U;
  bool ccdHasCompoundColliders = false;

  // Heap-backed so large heightfield buffers do not inflate World stack size.
  std::unique_ptr<PhysicsShapeStore> shapeStore;
};

} // namespace engine::physics
