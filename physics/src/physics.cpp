// Implements physics behavior for the Engine physics system.

#include "engine/physics/physics.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/math/aabb.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/physics/ccd.h"
#include "engine/physics/collider.h"
#include "engine/physics/constraint_solver.h"
#include "engine/physics/convex_hull.h"

#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"
#include "blocked_body_diagnostic.h"
#include "contact_resolution.h"
#include "narrow_phase.h"
#include "physics_internal.h"
#include "joint_handle.h"

namespace engine::physics {

namespace {

constexpr float kStaticInverseMass = 0.0F;
constexpr float kDefaultCellSize = 4.0F;
constexpr std::uint32_t kSpatialHashEmpty = 0xFFFFFFFFU;

constexpr std::uint8_t kSleepFramesRequired = 60U;

// The inverse mass a body answers a contact with. A sleeping body is
// static to every response path -- positional correction, speculative
// contacts and the impulse solve alike -- unless its partner is fast
// enough for record_pair_and_wake to wake it in this same response, in
// which case it answers with its real mass. The relaxation
// pass zeroes sleepers the same way; without this the primary response
// pushed a sleeper every step while it stayed marked asleep.
float effective_inverse_mass(const RigidBody *body,
                             const RigidBody *partner) noexcept {
  if (body == nullptr) {
    return kStaticInverseMass;
  }
  if (!body->sleeping) {
    return body->inverseMass;
  }
  const bool wokenByPartner =
      (partner != nullptr) &&
      (engine::math::length_sq(partner->velocity) > kSleepThreshold);
  return wokenByPartner ? body->inverseMass : kStaticInverseMass;
}

// Advances a stamp generation, clearing the stamps on wrap so stale marks
// can never read as current.
void begin_generation(std::uint32_t *generation, std::uint32_t *stamps,
                      std::size_t stampCount) noexcept {
  if ((generation == nullptr) || (stamps == nullptr)) {
    return;
  }

  ++(*generation);
  if (*generation != 0U) {
    return;
  }

  for (std::size_t i = 0U; i < stampCount; ++i) {
    stamps[i] = 0U;
  }
  *generation = 1U;
}

// Linked-list node for the broadphase spatial hash grid.

// Cell-quantization guards: coordinates clamp to +/-1e9 cells (inside
// int32) before the float-to-int cast so large-but-finite positions
// cannot invoke UB or unbounded loops, and any collider whose expanded
// bounds would touch more than kMaxCellsPerCollider cells (or that the
// node pool cannot hold) is diverted to the brute-force overflow list
// instead of silently losing grid coverage. 256 cells covers any body's
// travel -- kMaxLinearSpeed expansion is ~8.3 m per side against a 4 m
// cell, about 6 cells per axis -- and a collider over ~64 m across, such
// as a large ground box, overflows by design: it is tested against every
// collider, each pair costing one bounds comparison before any narrow
// phase.
constexpr float kMaxCellCoordMagnitude = 1.0e9F;
constexpr std::int32_t kMinCellCoord = -1000000000;
constexpr std::int32_t kMaxCellCoord = 1000000000;
constexpr std::int64_t kMaxCellsPerCollider = 256;

} // namespace

/// Publishes owners, live owner velocities, the identity slot map, and
/// open AABBs (gating must never reject when no real bounds exist yet) for
/// every collider, so the first step after world creation, scene load, or
/// an Input-phase add sweeps against moving targets instead of a static
/// world. Serial-only: races with the parallel chunk jobs otherwise.
void prime_ccd_snapshot(PhysicsWorldView &world) noexcept {
  PhysicsContext &physicsCtx = world.physics_context();
  if (!physicsCtx.ccdSnapshotDirty) {
    return;
  }
  PhysicsShapeStore *store = physicsCtx.shapeStore.get();
  if (store == nullptr) {
    return;
  }

  physicsCtx.ccdColliderCount = 0U;
  physicsCtx.ccdHasCompoundColliders = false;
  const std::size_t colliderCount = world.collider_count();
  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if ((colliderCount > 0U) &&
      !world.get_collider_range(0U, colliderCount, &entities, &colliders)) {
    return;
  }

  constexpr float kOpenBound = 1.0e30F;
  const math::AABB openBounds{math::Vec3(-kOpenBound, -kOpenBound, -kOpenBound),
                              math::Vec3(kOpenBound, kOpenBound, kOpenBound)};
  for (std::size_t i = 0U; i < colliderCount; ++i) {
    const Entity owner = world.rigid_body_owner(entities[i]);
    store->ccdColliderEntities[i] = entities[i];
    store->ccdColliderOwners[i] = owner;
    store->ccdColliderAabbs[i] = openBounds;
    const RigidBody *ownerBody = (owner != kInvalidEntity)
                                     ? world.get_rigid_body_ptr(owner)
                                     : nullptr;
    store->ccdColliderVelocities[i] =
        (ownerBody != nullptr) ? ownerBody->velocity
                               : math::Vec3(0.0F, 0.0F, 0.0F);
    if (entities[i].index < store->ccdSlotByEntityIndex.size()) {
      store->ccdSlotByEntityIndex[entities[i].index] =
          static_cast<std::uint32_t>(i);
    }
    if ((owner != kInvalidEntity) && (owner != entities[i])) {
      physicsCtx.ccdHasCompoundColliders = true;
    }
  }
  physicsCtx.ccdColliderCount = colliderCount;
  physicsCtx.ccdSnapshotDirty = false;
}

/// Broadphase + narrow phase + constraint solve + sleep for one step:
/// collider world geometry is built serially in dense order (the only
/// simulation stage allowed to compose child transforms from the write
/// buffer), the CCD ownership/bounds snapshot is published (including the
/// entity-index -> slot map that keeps next-step lookups identity-based
/// across sparse-set reorders), colliders hash into a spatial grid with
/// AABBs expanded by velocity*dt so approaching pairs surface for
/// speculative contacts — the SAME expansion applies to both the insert
/// and the scan passes, so pair discovery cannot depend on which side of
/// the pair holds the smaller dense index — cell coordinates quantize
/// through a clamped int32 conversion, colliders whose cell span or node
/// budget overflows divert to a lossless brute-force overflow list (one
/// warning per episode), shape-pair testers run per unique pair, then
/// joints solve and bodies below the energy threshold long enough are put
/// to sleep.
bool resolve_collisions(PhysicsWorldView &world, float deltaSeconds) noexcept {
  const auto simToken = world.simulation_access_token();
  PhysicsContext &physicsCtx = world.physics_context();
  if (!step_delta_is_valid(deltaSeconds)) {
    core::log_message(core::LogLevel::Error, "physics",
                      "resolve_collisions rejected a non-finite or "
                      "non-positive delta; bodies and contacts are unchanged");
    return false;
  }

  physicsCtx.collisionPairCount = 0U;
  physicsCtx.collisionPairDropCount = 0U;
  physicsCtx.narrowPhasePairTests = 0U;
  ++physicsCtx.solverFrameNumber;
  begin_generation(&physicsCtx.pairHashGeneration,
                   physicsCtx.pairHashStamps.data(),
                   physicsCtx.pairHashStamps.size());

  const std::size_t colliderCount = world.collider_count();

  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if ((colliderCount > 0U) &&
      !world.get_collider_range(0U, colliderCount, &entities, &colliders)) {
    return false;
  }

  // Broadphase dedupe stamps live in the heap-backed shape store; fetched
  // once so the per-collider loop below never re-derefs the unique_ptr.
  PhysicsShapeStore *const shapeStorePtr = physicsCtx.shapeStore.get();

  // The workspace is context-owned — allocated once per physics
  // context on its first resolve (never per step, never per thread) and
  // freed with the World.
  if (physicsCtx.resolveScratch == nullptr) {
    physicsCtx.resolveScratch.reset(new (std::nothrow) ResolveScratch());
  }
  ResolveScratch *const resolveScratch = physicsCtx.resolveScratch.get();
  if (resolveScratch == nullptr) {
    core::log_message(core::LogLevel::Error, "physics",
                      "resolve_collisions scratch allocation failed");
    return false;
  }

  capture_blocked_body_commands(world);

  auto &geometries = resolveScratch->geometries;
  auto &bodyOwners = resolveScratch->bodyOwners;
  auto &bodyCenters = resolveScratch->bodyCenters;
  auto &bodyRotations = resolveScratch->bodyRotations;
  auto &geometryValid = resolveScratch->geometryValid;
  auto &posX = resolveScratch->posX;
  auto &posY = resolveScratch->posY;
  auto &posZ = resolveScratch->posZ;

  for (std::size_t i = 0U; i < colliderCount; ++i) {
    PhysicsTransform entityTransform{};
    const ConvexHullData *hull = nullptr;
    if (colliders[i].shape == ColliderShape::ConvexHull) {
      hull = find_hull_data(physicsCtx, entities[i]);
    }

    geometryValid[i] =
        world.get_simulation_physics_transform(entities[i], simToken,
                                               &entityTransform) &&
        make_collider_world_geometry(colliders[i], entityTransform.matrix, hull,
                                     &geometries[i]);
    bodyOwners[i] = kInvalidEntity;
    bodyCenters[i] = engine::math::Vec3(0.0F, 0.0F, 0.0F);
    bodyRotations[i] = engine::math::Quat();
    posX[i] = 0.0F;
    posY[i] = 0.0F;
    posZ[i] = 0.0F;
    if (!geometryValid[i]) {
      continue;
    }

    posX[i] = geometries[i].center.x;
    posY[i] = geometries[i].center.y;
    posZ[i] = geometries[i].center.z;
    bodyOwners[i] = world.rigid_body_owner(entities[i], simToken);
    if (bodyOwners[i] == kInvalidEntity) {
      bodyCenters[i] = geometries[i].center;
      continue;
    }

    PhysicsTransform bodyTransform{};
    if (!world.get_simulation_physics_transform(bodyOwners[i], simToken,
                                                &bodyTransform)) {
      geometryValid[i] = false;
      bodyOwners[i] = kInvalidEntity;
      continue;
    }
    bodyCenters[i] = bodyTransform.position;
    bodyRotations[i] = engine::math::normalize(bodyTransform.rotation);
  }

  physicsCtx.ccdColliderCount = 0U;
  physicsCtx.ccdHasCompoundColliders = false;
  if (shapeStorePtr != nullptr) {
    PhysicsShapeStore &store = *shapeStorePtr;
    physicsCtx.ccdColliderCount = colliderCount;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      store.ccdColliderEntities[i] = entities[i];
      store.ccdColliderOwners[i] = bodyOwners[i];
      // Publish a zeroed AABB for invalid geometry: geometries[i] is
      // per-thread scratch, so its stale content would vary with whichever
      // worker ran the previous resolve.
      store.ccdColliderAabbs[i] =
          geometryValid[i] ? geometries[i].worldAabb : math::AABB{};
      if (entities[i].index < store.ccdSlotByEntityIndex.size()) {
        store.ccdSlotByEntityIndex[entities[i].index] =
            static_cast<std::uint32_t>(i);
      }
      if ((bodyOwners[i] != kInvalidEntity) &&
          (bodyOwners[i] != entities[i])) {
        physicsCtx.ccdHasCompoundColliders = true;
      }
    }
  }

  bool stepHadOverflow = false;
  // The dedupe stamp array is shapeStore-backed; without a store, pair
  // discovery cannot dedupe neighbors so broadphase is skipped this step
  // (mirrors the CCD-snapshot degrade above under the same OOM condition).
  if ((colliderCount >= 2U) && (shapeStorePtr != nullptr)) {
    std::uint32_t *const testedStamps = shapeStorePtr->testedStamps.data();
    const std::size_t testedStampsSize = shapeStorePtr->testedStamps.size();

    // A fixed cell: sized from the largest collider, one big ground box
    // put every collider in one cell and made pair testing quadratic. A
    // collider too big for its share of cells goes to the overflow list
    // below, which is lossless.
    const float invCellSize = 1.0F / kDefaultCellSize;

    auto &buckets = resolveScratch->buckets;
    auto &nodes = resolveScratch->nodes;
    auto &expandX = resolveScratch->expandX;
    auto &expandY = resolveScratch->expandY;
    auto &expandZ = resolveScratch->expandZ;
    auto &overflowList = resolveScratch->overflowList;
    auto &isOverflow = resolveScratch->isOverflow;
    std::size_t nodeCount = 0U;
    std::size_t overflowCount = 0U;

    for (std::size_t b = 0U; b < kSpatialHashBuckets; ++b) {
      buckets[b] = kSpatialHashEmpty;
    }

    auto cell_coord = [invCellSize](float v) noexcept -> std::int32_t {
      const float scaled = std::floor(v * invCellSize);
      if (!(scaled >= -kMaxCellCoordMagnitude)) {
        return kMinCellCoord;
      }
      if (scaled > kMaxCellCoordMagnitude) {
        return kMaxCellCoord;
      }
      return static_cast<std::int32_t>(scaled);
    };

    auto hash_cell = [](std::int32_t cx, std::int32_t cy,
                        std::int32_t cz) noexcept -> std::uint32_t {
      auto u = static_cast<std::uint32_t>(cx) * 73856093U ^
               static_cast<std::uint32_t>(cy) * 19349663U ^
               static_cast<std::uint32_t>(cz) * 83492791U;
      return u % static_cast<std::uint32_t>(kSpatialHashBuckets);
    };

    auto insert_node = [&](std::uint32_t bucket,
                           std::uint32_t colIdx) noexcept -> bool {
      if (nodeCount >= kMaxNodes) {
        return false;
      }
      nodes[nodeCount] = {colIdx, buckets[bucket]};
      buckets[bucket] = static_cast<std::uint32_t>(nodeCount);
      ++nodeCount;
      return true;
    };

    const float speculativeDt = deltaSeconds;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      expandX[i] = 0.0F;
      expandY[i] = 0.0F;
      expandZ[i] = 0.0F;
      isOverflow[i] = false;
      if (!geometryValid[i]) {
        continue;
      }

      const Entity bodyOwner = bodyOwners[i];
      const RigidBody *bodyI = (bodyOwner != kInvalidEntity)
                                   ? world.get_rigid_body_ptr(bodyOwner)
                                   : nullptr;
      if ((bodyI != nullptr) && (bodyI->inverseMass > 0.0F)) {
        const engine::math::Vec3 centerOffset =
            engine::math::sub(geometries[i].center, bodyCenters[i]);
        const engine::math::Vec3 pointVelocity = engine::math::add(
            bodyI->velocity,
            engine::math::cross(bodyI->angularVelocity, centerOffset));
        expandX[i] = std::fabs(pointVelocity.x) * speculativeDt;
        expandY[i] = std::fabs(pointVelocity.y) * speculativeDt;
        expandZ[i] = std::fabs(pointVelocity.z) * speculativeDt;
      }

      const engine::math::AABB &bounds = geometries[i].worldAabb;
      const std::int32_t minCX = cell_coord(bounds.min.x - expandX[i]);
      const std::int32_t maxCX = cell_coord(bounds.max.x + expandX[i]);
      const std::int32_t minCY = cell_coord(bounds.min.y - expandY[i]);
      const std::int32_t maxCY = cell_coord(bounds.max.y + expandY[i]);
      const std::int32_t minCZ = cell_coord(bounds.min.z - expandZ[i]);
      const std::int32_t maxCZ = cell_coord(bounds.max.z + expandZ[i]);
      const std::int64_t cellTotal =
          (static_cast<std::int64_t>(maxCX) - minCX + 1) *
          (static_cast<std::int64_t>(maxCY) - minCY + 1) *
          (static_cast<std::int64_t>(maxCZ) - minCZ + 1);

      bool inserted = (cellTotal >= 1) && (cellTotal <= kMaxCellsPerCollider);
      if (inserted) {
        for (std::int32_t cx = minCX; inserted && (cx <= maxCX); ++cx) {
          for (std::int32_t cy = minCY; inserted && (cy <= maxCY); ++cy) {
            for (std::int32_t cz = minCZ; inserted && (cz <= maxCZ); ++cz) {
              inserted = insert_node(hash_cell(cx, cy, cz),
                                     static_cast<std::uint32_t>(i));
            }
          }
        }
      }
      if (!inserted) {
        isOverflow[i] = true;
        overflowList[overflowCount] = static_cast<std::uint32_t>(i);
        ++overflowCount;
      }
    }
    stepHadOverflow = overflowCount > 0U;

    for (std::size_t i = 0U; i < colliderCount; ++i) {
      if (!geometryValid[i]) {
        continue;
      }
      const Entity entityA = entities[i];
      const Entity authorityEntityA =
          (bodyOwners[i] != kInvalidEntity) ? bodyOwners[i] : entityA;
      if (world.movement_authority(authorityEntityA) ==
          MovementAuthority::Script) {
        continue;
      }

      const float ax = posX[i];
      const float ay = posY[i];
      const float az = posZ[i];

      begin_generation(&physicsCtx.testedGeneration, testedStamps,
                       testedStampsSize);
      testedStamps[i] = physicsCtx.testedGeneration;

      auto test_pair = [&](std::uint32_t jIndex) noexcept {
        const std::size_t j = jIndex;
        if (testedStamps[j] == physicsCtx.testedGeneration) {
          return;
        }
        testedStamps[j] = physicsCtx.testedGeneration;

        if (j <= i) {
          return;
        }

        const Entity entityB = entities[j];
        if (!geometryValid[j]) {
          return;
        }

        // Only a pair whose bounds, each grown by its body's travel over
        // the step, overlap can touch within it; anything else would reach
        // a narrow phase only because the grid put it in the same cell.
        const engine::math::AABB &boundsI = geometries[i].worldAabb;
        const engine::math::AABB &boundsJ = geometries[j].worldAabb;
        if (((boundsI.max.x + expandX[i]) < (boundsJ.min.x - expandX[j])) ||
            ((boundsJ.max.x + expandX[j]) < (boundsI.min.x - expandX[i])) ||
            ((boundsI.max.y + expandY[i]) < (boundsJ.min.y - expandY[j])) ||
            ((boundsJ.max.y + expandY[j]) < (boundsI.min.y - expandY[i])) ||
            ((boundsI.max.z + expandZ[i]) < (boundsJ.min.z - expandZ[j])) ||
            ((boundsJ.max.z + expandZ[j]) < (boundsI.min.z - expandZ[i]))) {
          return;
        }
        const Entity authorityEntityB =
            (bodyOwners[j] != kInvalidEntity) ? bodyOwners[j] : entityB;
        if (world.movement_authority(authorityEntityB) ==
            MovementAuthority::Script) {
          return;
        }

        if ((bodyOwners[i] != kInvalidEntity) &&
            (bodyOwners[i] == bodyOwners[j])) {
          return;
        }

        const float bx = posX[j];
        const float by = posY[j];
        const float bz = posZ[j];

        const Collider &colliderA = colliders[i];
        const Collider &colliderB = colliders[j];

        if (((colliderA.collisionLayer & colliderB.collisionMask) == 0U) ||
            ((colliderB.collisionLayer & colliderA.collisionMask) == 0U)) {
          return;
        }

        const Entity bodyEntityA = bodyOwners[i];
        const Entity bodyEntityB = bodyOwners[j];
        RigidBody *bodyA = (bodyEntityA != kInvalidEntity)
                               ? world.get_rigid_body_ptr(bodyEntityA)
                               : nullptr;
        RigidBody *bodyB = (bodyEntityB != kInvalidEntity)
                               ? world.get_rigid_body_ptr(bodyEntityB)
                               : nullptr;
        const float invMassA = effective_inverse_mass(bodyA, bodyB);
        const float invMassB = effective_inverse_mass(bodyB, bodyA);
        const float invMassSum = invMassA + invMassB;

        const auto shapeA = colliderA.shape;
        const auto shapeB = colliderB.shape;
        const bool aIsAABB = (shapeA == ColliderShape::AABB);
        const bool bIsAABB = (shapeB == ColliderShape::AABB);
        const bool aIsCapsule = (shapeA == ColliderShape::Capsule);
        const bool bIsCapsule = (shapeB == ColliderShape::Capsule);
        const bool aIsSphere = (shapeA == ColliderShape::Sphere);
        const bool bIsSphere = (shapeB == ColliderShape::Sphere);
        const bool aIsConvex = (shapeA == ColliderShape::ConvexHull);
        const bool bIsConvex = (shapeB == ColliderShape::ConvexHull);
        const bool aIsHeightfield = (shapeA == ColliderShape::Heightfield);
        const bool bIsHeightfield = (shapeB == ColliderShape::Heightfield);

        const bool compoundA =
            (bodyEntityA != kInvalidEntity) && (bodyEntityA != entityA);
        const bool compoundB =
            (bodyEntityB != kInvalidEntity) && (bodyEntityB != entityB);
        const bool offsetBodyA =
            (bodyEntityA != kInvalidEntity) &&
            (engine::math::length_sq(engine::math::sub(
                 geometries[i].center, bodyCenters[i])) > 1.0e-12F);
        const bool offsetBodyB =
            (bodyEntityB != kInvalidEntity) &&
            (engine::math::length_sq(engine::math::sub(
                 geometries[j].center, bodyCenters[j])) > 1.0e-12F);
        const bool requiresAffineNarrowPhase =
            compoundA || compoundB || offsetBodyA || offsetBodyB ||
            has_non_identity_linear_transform(geometries[i]) ||
            has_non_identity_linear_transform(geometries[j]);

        ++physicsCtx.narrowPhasePairTests;
        const PairContext pair{world,
                               simToken,
                               physicsCtx,
                               entityA,
                               entityB,
                               bodyEntityA,
                               bodyEntityB,
                               colliderA,
                               colliderB,
                               geometries[i],
                               geometries[j],
                               bodyA,
                               bodyB,
                               invMassA,
                               invMassB,
                               invMassSum,
                               engine::math::Vec3(ax, ay, az),
                               engine::math::Vec3(bx, by, bz),
                               bodyCenters[i],
                               bodyCenters[j],
                               bodyRotations[i],
                               bodyRotations[j],
                               requiresAffineNarrowPhase,
                               speculativeDt};

        if (aIsHeightfield || bIsHeightfield) {
          narrow_phase_heightfield(pair);
          return;
        }
        if (requiresAffineNarrowPhase || aIsConvex || bIsConvex) {
          narrow_phase_convex_gjk(pair);
          return;
        }

        if (aIsCapsule && bIsCapsule) {
          narrow_phase_capsule_capsule(pair);
          return;
        }

        if ((aIsCapsule && bIsSphere) || (aIsSphere && bIsCapsule)) {
          narrow_phase_capsule_sphere(pair);
          return;
        }

        if ((aIsCapsule && bIsAABB) || (aIsAABB && bIsCapsule)) {
          narrow_phase_capsule_aabb(pair);
          return;
        }

        if (aIsSphere && bIsSphere) {
          narrow_phase_sphere_sphere(pair);
          return;
        }

        if (aIsAABB != bIsAABB) {
          narrow_phase_aabb_sphere(pair);
          return;
        }

        narrow_phase_aabb_aabb(pair);
      };

      if (isOverflow[i]) {
        for (std::size_t j = i + 1U; j < colliderCount; ++j) {
          test_pair(static_cast<std::uint32_t>(j));
        }
        continue;
      }

      // Candidates are gathered first and tested in index order, so the
      // order pairs resolve in -- which the result depends on -- follows
      // the colliders, not the grid's bucket layout.
      auto &candidates = resolveScratch->candidates;
      std::size_t candidateCount = 0U;
      const auto gather = [&](std::uint32_t j) noexcept {
        if ((j > i) && (testedStamps[j] != physicsCtx.testedGeneration)) {
          testedStamps[j] = physicsCtx.testedGeneration;
          candidates[candidateCount] = j;
          ++candidateCount;
        }
      };

      const engine::math::AABB &boundsA = geometries[i].worldAabb;
      const std::int32_t minCX = cell_coord(boundsA.min.x - expandX[i]);
      const std::int32_t maxCX = cell_coord(boundsA.max.x + expandX[i]);
      const std::int32_t minCY = cell_coord(boundsA.min.y - expandY[i]);
      const std::int32_t maxCY = cell_coord(boundsA.max.y + expandY[i]);
      const std::int32_t minCZ = cell_coord(boundsA.min.z - expandZ[i]);
      const std::int32_t maxCZ = cell_coord(boundsA.max.z + expandZ[i]);

      for (std::int32_t cx = minCX; cx <= maxCX; ++cx) {
        for (std::int32_t cy = minCY; cy <= maxCY; ++cy) {
          for (std::int32_t cz = minCZ; cz <= maxCZ; ++cz) {
            const std::uint32_t bucket = hash_cell(cx, cy, cz);
            std::uint32_t nodeIdx = buckets[bucket];
            while (nodeIdx != kSpatialHashEmpty) {
              const std::uint32_t j = nodes[nodeIdx].colliderIdx;
              nodeIdx = nodes[nodeIdx].next;
              gather(j);
            }
          }
        }
      }

      for (std::size_t o = 0U; o < overflowCount; ++o) {
        gather(overflowList[o]);
      }

      std::sort(candidates.begin(),
                candidates.begin() +
                    static_cast<std::ptrdiff_t>(candidateCount));
      // test_pair dedupes on the same stamps gather set; a fresh
      // generation lets it take each candidate exactly once.
      begin_generation(&physicsCtx.testedGeneration, testedStamps,
                       testedStampsSize);
      testedStamps[i] = physicsCtx.testedGeneration;
      for (std::size_t c = 0U; c < candidateCount; ++c) {
        test_pair(candidates[c]);
      }
    }
  }

  if (stepHadOverflow) {
    if (!physicsCtx.broadphaseOverflowActive) {
      physicsCtx.broadphaseOverflowActive = true;
      ++physicsCtx.broadphaseOverflowEpisodes;
      core::log_message(core::LogLevel::Warning, "physics",
                        "broad-phase cell budget exceeded; overflowed "
                        "colliders fall back to exhaustive pair tests");
    }
  } else {
    physicsCtx.broadphaseOverflowActive = false;
  }

  // Append this step's kept pairs to the frame buffer in step order.
  std::uint32_t frameAppendDropCount = 0U;
  for (std::size_t i = 0U; i < physicsCtx.collisionPairCount; ++i) {
    if (physicsCtx.frameCollisionPairCount >=
        (kMaxCollisionPairs * kMaxCollisionFrameSteps)) {
      ++frameAppendDropCount;
      continue;
    }
    const std::size_t dst = physicsCtx.frameCollisionPairCount * 2U;
    physicsCtx.frameCollisionPairData[dst] =
        physicsCtx.collisionPairData[i * 2U];
    physicsCtx.frameCollisionPairData[dst + 1U] =
        physicsCtx.collisionPairData[(i * 2U) + 1U];
    ++physicsCtx.frameCollisionPairCount;
  }
  physicsCtx.frameCollisionPairDropCount +=
      physicsCtx.collisionPairDropCount + frameAppendDropCount;

  if ((physicsCtx.collisionPairDropCount > 0U) ||
      (frameAppendDropCount > 0U)) {
    if (!physicsCtx.collisionPairOverflowActive) {
      physicsCtx.collisionPairOverflowActive = true;
      ++physicsCtx.collisionPairOverflowEpisodes;
      core::log_message(core::LogLevel::Warning, "physics",
                        "collision pair buffer full; contacts past "
                        "kMaxCollisionPairs report no callbacks this episode");
    }
  } else {
    physicsCtx.collisionPairOverflowActive = false;
  }

  // Extra outer passes over this frame's cached contacts:
  // propagates corrections through contact chains (stacks) within this step
  // instead of leaving convergence to accumulate one frame at a time via
  // warm start alone. Runs before joints solve, mirroring the primary
  // resolve's contacts-then-joints order.
  relax_cached_contacts(world, simToken, physicsCtx);

  solve_constraints(world, deltaSeconds);

  const std::size_t rigidBodyCount = world.rigid_body_count();
  const Entity *rigidBodyEntities = nullptr;
  RigidBody *rigidBodies = nullptr;
  if ((rigidBodyCount > 0U) &&
      !world.get_rigid_body_range(0U, rigidBodyCount, &rigidBodyEntities,
                                  &rigidBodies)) {
    return false;
  }
  (void)rigidBodyEntities;
  for (std::size_t i = 0U; i < rigidBodyCount; ++i) {
    RigidBody *body = &rigidBodies[i];
    if ((body->inverseMass <= 0.0F) || body->sleeping) {
      continue;
    }
    const float energy = engine::math::length_sq(body->velocity) +
                         engine::math::length_sq(body->angularVelocity);
    if (energy < kSleepThreshold) {
      if (body->sleepFrameCount >= kSleepFramesRequired) {
        body->sleeping = true;
        body->velocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
        body->angularVelocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
      } else {
        ++body->sleepFrameCount;
      }
    } else {
      body->sleepFrameCount = 0U;
    }
  }

  // Capture owner velocities into the CCD snapshot LAST: the next step's CCD
  // consumes them instead of live RigidBody reads (which race with parallel
  // chunk integration), so they must include this step's solver impulses.
  if (physicsCtx.shapeStore != nullptr) {
    PhysicsShapeStore &store = *physicsCtx.shapeStore;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const RigidBody *ownerBody =
          (bodyOwners[i] != kInvalidEntity)
              ? world.get_rigid_body_ptr(bodyOwners[i])
              : nullptr;
      store.ccdColliderVelocities[i] = (ownerBody != nullptr)
                                           ? ownerBody->velocity
                                           : math::Vec3(0.0F, 0.0F, 0.0F);
    }
    physicsCtx.ccdSnapshotDirty = false;
  }

  // Manifolds no pair touched this resolve hold contacts that no longer
  // exist; drop them so warm starts never replay a vanished contact.
  manifold_evict_stale(physicsCtx, physicsCtx.solverFrameNumber);

  report_blocked_bodies(world, deltaSeconds);

  return true;
}

/// Sets the requested value for gravity.
void set_gravity(PhysicsWorldView &world, float x, float y, float z) noexcept {
  world.physics_context().gravity = engine::math::Vec3(x, y, z);
}

engine::math::Vec3 get_gravity(const PhysicsWorldView &world) noexcept {
  return world.physics_context().gravity;
}

/// Sets the requested value for collision dispatch.
void set_collision_dispatch(PhysicsWorldView &world,
                            CollisionDispatchFn fn) noexcept {
  world.physics_context().collisionDispatch = fn;
}

// Drains the frame-accumulated pairs so every catch-up step's callbacks
// reach the dispatch in step order once per rendered frame.
void dispatch_collision_callbacks(PhysicsWorldView &world) noexcept {
  PhysicsContext &ctx = world.physics_context();
  if ((ctx.collisionDispatch != nullptr) &&
      (ctx.frameCollisionPairCount > 0U)) {
    ctx.collisionDispatch(ctx.frameCollisionPairData.data(),
                          ctx.frameCollisionPairCount);
  }
  ctx.frameCollisionPairCount = 0U;
  ctx.frameCollisionPairDropCount = 0U;
  ctx.collisionPairCount = 0U;
}

JointId add_distance_joint(PhysicsWorldView &world, Entity entityA,
                           Entity entityB, float distance) noexcept {
  Transform transformA{};
  Transform transformB{};
  if (!std::isfinite(distance) || (distance < 0.0F) ||
      (entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      (entityA == entityB) || !world.get_transform(entityA, &transformA) ||
      !world.get_transform(entityB, &transformB)) {
    core::log_message(core::LogLevel::Error, "physics",
                      "invalid distance joint endpoints or distance");
    return kInvalidJointId;
  }

  PhysicsJointSlot *joint = nullptr;
  PhysicsContext &context = world.physics_context();
  const JointId id = claim_joint_slot(context, &joint);
  if ((id == kInvalidJointId) || (joint == nullptr)) {
    core::log_message(core::LogLevel::Error, "physics", "joint table full");
    return kInvalidJointId;
  }

  joint->entityA = entityA;
  joint->entityB = entityB;
  joint->type = JointType::Distance;
  joint->distance = distance;
  joint->active = true;
  joint->accumulatedImpulse = 0.0F;
  return id;
}

bool remove_joint(PhysicsWorldView &world, JointId id) noexcept {
  PhysicsContext &context = world.physics_context();
  std::size_t slotIndex = 0U;
  PhysicsJointSlot *joint = find_joint_slot(context, id, &slotIndex);
  if (joint == nullptr) {
    return false;
  }

  retire_joint_slot(*joint);
  // find_joint_slot only returns non-null when shapeStore is live, so the
  // store lookup here cannot fail.
  PhysicsShapeStore *store = context.shapeStore.get();
  auto &joints = store->joints;
  while ((context.jointCount > 0U) && !joints[context.jointCount - 1U].active) {
    --context.jointCount;
  }
  return true;
}

void remove_joints_for_entity(PhysicsContext &context, Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if ((store == nullptr) || (context.jointCount == 0U)) {
    return;
  }
  auto &joints = store->joints;
  for (std::size_t i = 0U; i < context.jointCount; ++i) {
    PhysicsJointSlot &joint = joints[i];
    if (joint.active &&
        ((joint.entityA == entity) || (joint.entityB == entity))) {
      retire_joint_slot(joint);
    }
  }
  while ((context.jointCount > 0U) && !joints[context.jointCount - 1U].active) {
    --context.jointCount;
  }
}

void reset_physics_content(PhysicsContext &context) noexcept {
  context.gravity = kDefaultGravity;
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store != nullptr) {
    // Retire rather than clear so a JointId held across the reset stays
    // stale instead of resolving to a joint the next scene creates.
    for (std::size_t i = 0U; i < context.jointCount; ++i) {
      if (store->joints[i].active) {
        retire_joint_slot(store->joints[i]);
      }
    }
  }
  context.jointCount = 0U;
}

void wake_body(PhysicsWorldView &world, Entity entity) noexcept {
  RigidBody *body = world.get_rigid_body_ptr(entity);
  if (body != nullptr) {
    body->sleeping = false;
    body->sleepFrameCount = 0U;
  }
}

/// Returns whether is sleeping.
bool is_sleeping(const PhysicsWorldView &world, Entity entity) noexcept {
  RigidBody rb{};
  if (!world.get_rigid_body(entity, &rb)) {
    return false;
  }
  return rb.sleeping;
}

} // namespace engine::physics