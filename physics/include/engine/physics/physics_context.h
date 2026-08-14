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
#include "engine/physics/constraint_solver.h"
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
// Fixed catch-up steps one rendered frame may accumulate callbacks for;
// must cover the pipeline's step cap so accumulation alone never drops.
static constexpr std::size_t kMaxCollisionFrameSteps = 8U;
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

  // Angle or distance limits (hinge: radians within [-pi, pi], slider:
  // distance).
  float minLimit = 0.0F;
  float maxLimit = 0.0F;

  // Hinge limit tracking: the continuous (unwrapped) twist accumulated
  // from shortest-arc deltas of the wrapped atan2 measurement, so limits
  // near +/-pi clamp against the boundary the body actually crossed
  // instead of the one the wrap teleports it to. twistTracked latches
  // after the first measurement seeds the accumulator.
  float twistContinuous = 0.0F;
  bool twistTracked = false;

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
  // Joint slot table and broadphase dedupe stamps (issue #129): moved off
  // PhysicsContext itself, which a Windows main-red incident found sat ~8 KB
  // under the platform's 1 MB default thread stack when stack-constructed.
  std::array<PhysicsJointSlot, kMaxPhysicsJoints> joints{};
  std::array<std::uint32_t, kMaxColliders> testedStamps{};

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
  // Entity-index -> snapshot-slot map so CCD matches snapshot entries by
  // identity instead of dense position: sparse-set reorders between the
  // publish and the next step's sweep would otherwise mismatch every
  // shifted entry. Stale slots are harmless — a lookup is valid only when
  // the slot is in range AND ccdColliderEntities[slot] equals the queried
  // entity (index and generation), so entries are overwritten, never
  // cleared.
  std::array<std::uint32_t, ENGINE_MAX_ENTITIES> ccdSlotByEntityIndex{};
  // Owner body velocities captured with the snapshot: CCD's candidate
  // rejection must not read live RigidBody::velocity, which parallel chunk
  // jobs are integrating concurrently.
  std::array<math::Vec3, kMaxColliders> ccdColliderVelocities{};

  // Persistent contact-manifold cache (issue #110): world-scoped so
  // separate worlds never share warm-start state; entries are keyed by
  // full Entity so index reuse cannot inherit stale impulses.
  std::array<ContactManifold, kMaxContactManifolds> contactManifolds{};
  std::size_t contactManifoldCount = 0U;
  // O(1) pair->slot index (open addressing over entity-index pair keys;
  // hits verify full Entity identity). Maintained on insert, rebuilt after
  // eviction compaction, so per-pair lookups never scan the whole cache.
  std::array<std::uint32_t, kManifoldHashBuckets> contactManifoldHash = [] {
    std::array<std::uint32_t, kManifoldHashBuckets> filled{};
    filled.fill(kManifoldSlotEmpty);
    return filled;
  }();
  // Resolve frame in which the cache was found full of live manifolds:
  // further pairs that frame cold-start instead of thrashing evictions.
  std::uint32_t contactManifoldSaturatedFrame = 0U;

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
  // Slot table lives in shapeStore; joint-adding code must never let
  // jointCount go nonzero without a live store (see joint_handle.h).
  std::size_t jointCount = 0U;

  // Packed collision pairs: [entityIndexA0, entityIndexB0, ...]
  std::array<std::uint32_t, kMaxCollisionPairs * 2U> collisionPairData{};
  std::size_t collisionPairCount = 0U;
  CollisionDispatchFn collisionDispatch = nullptr;

  // Frame-accumulated pairs (issue #103): every fixed step appends its kept
  // pairs in step order and dispatch drains once per rendered frame, so a
  // pair persisting across substeps repeats once per substep. Drops are
  // counted per rendered frame (per-step caps plus append overflow).
  std::array<std::uint32_t,
             kMaxCollisionPairs * kMaxCollisionFrameSteps * 2U>
      frameCollisionPairData{};
  std::size_t frameCollisionPairCount = 0U;
  std::uint32_t frameCollisionPairDropCount = 0U;

  // O(1) collision-pair dedupe via open addressing and generation stamps.
  std::array<std::uint64_t, kCollisionPairHashBuckets> pairHashKeys{};
  std::array<std::uint32_t, kCollisionPairHashBuckets> pairHashStamps{};
  std::uint32_t pairHashGeneration = 1U;

  // O(1) broadphase neighbor dedupe using per-collider generation stamps;
  // the stamp array itself lives in shapeStore (see kMaxColliders comment
  // above joints in PhysicsShapeStore).
  std::uint32_t testedGeneration = 1U;

  // Valid entry count and compound flag for the shape store's CCD snapshot
  // (one step stale after reparenting, an Input-phase-only mutation). When
  // no compound colliders exist, CCD sweeps each body's own collider.
  std::size_t ccdColliderCount = 0U;
  bool ccdHasCompoundColliders = false;

  // True until resolve_collisions publishes a snapshot (fresh world, scene
  // load) or after Input-phase collider/body adds: the serial begin-step
  // path then primes a conservative snapshot so the first sweep sees
  // moving targets instead of a static world (issue #106).
  bool ccdSnapshotDirty = true;

  // Monotonic resolve counter stamping manifold-cache use for eviction.
  std::uint32_t solverFrameNumber = 0U;

  // Broad-phase overflow diagnostic: overflowActive is set while any
  // collider is served by the brute-force overflow list (per-collider
  // cell-span cap or spatial-node pool exhaustion) and logs once per
  // episode; the episode counter is observable by tests. Overflowed
  // colliders lose no pairs — they are tested against every collider.
  bool broadphaseOverflowActive = false;
  std::uint32_t broadphaseOverflowEpisodes = 0U;

  // Collision-pair buffer diagnostic: pairs recorded past
  // kMaxCollisionPairs are counted per step and reported once per
  // overflow episode. The kept set is the first kMaxCollisionPairs in
  // deterministic traversal order; callbacks past the cap are dropped
  // (loudly), never reordered.
  std::uint32_t collisionPairDropCount = 0U;
  bool collisionPairOverflowActive = false;
  std::uint32_t collisionPairOverflowEpisodes = 0U;

  // Raw per-step cvar cache written by refresh_step_cvar_cache on the
  // serial begin-step path so step/resolve code never takes the global
  // cvar mutex inside the parallel section; each consumer keeps its own
  // validation. Defaults mirror the registered cvar defaults.
  float ccdThresholdCvar = 2.0F;
  float blockedWarnStepsCvar = 30.0F;
  int solverIterationsCvar = 8;

  // Heap-backed so large heightfield buffers do not inflate World stack size.
  std::unique_ptr<PhysicsShapeStore> shapeStore;
};

// Compile-time regrowth guard for issue #129: PhysicsContext used to sit at
// ~1,016 KB (joints[4096] + testedStamps[65536] dominated), ~8 KB under
// Windows' 1 MB default thread stack, and a stack-constructed instance (the
// manifold suite's original fixture) segfaulted there across four CI runs.
// Both arrays now live in the heap-backed PhysicsShapeStore; this budget
// catches any future field that regrows the struct toward that trap before
// it ships. Raise it only with a matching stack-safety review.
static_assert(sizeof(PhysicsContext) <= 200U * 1024U,
             "PhysicsContext regrew toward the Windows stack-overflow "
             "incident in issue #129; move new large arrays into "
             "PhysicsShapeStore instead of raising this budget");

} // namespace engine::physics
