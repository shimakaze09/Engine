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
constexpr std::size_t kSpatialHashBuckets = 4096U;
constexpr std::uint32_t kSpatialHashEmpty = 0xFFFFFFFFU;

constexpr std::uint8_t kSleepFramesRequired = 60U;

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
struct SpatialNode final {
  std::uint32_t colliderIdx;
  std::uint32_t next;
};

// Max spatial-hash entries: each collider may touch up to 8 cells (the
// corners of its AABB).
constexpr std::size_t kMaxNodes = kMaxColliders * 8U;

// Per-thread scratch buffers for resolve_collisions, heap-allocated on first
// use. These must not be plain thread_local arrays: ~19 MB of static TLS is
// carved out of every new thread's stack allocation on glibc, which starves
// threads created with small explicit stacks (Mesa's GL driver workers
// overflowed and crashed the editor on startup exactly that way).
struct ResolveScratch final {
  std::array<ColliderWorldGeometry, kMaxColliders> geometries{};
  std::array<Entity, kMaxColliders> bodyOwners{};
  std::array<engine::math::Vec3, kMaxColliders> bodyCenters{};
  std::array<bool, kMaxColliders> geometryValid{};
  std::array<float, kMaxColliders> posX{};
  std::array<float, kMaxColliders> posY{};
  std::array<float, kMaxColliders> posZ{};
  std::array<std::uint32_t, kSpatialHashBuckets> buckets{};
  std::array<SpatialNode, kMaxNodes> nodes{};
};

// Owns one thread's ResolveScratch and frees it at thread exit.
struct ResolveScratchOwner final {
  ResolveScratch *scratch = nullptr;
  ResolveScratchOwner() noexcept = default;
  ResolveScratchOwner(const ResolveScratchOwner &) = delete;
  ResolveScratchOwner &operator=(const ResolveScratchOwner &) = delete;
  ~ResolveScratchOwner() { delete scratch; }
};

// Returns this thread's resolve scratch, allocating it on first use (a
// one-time per-thread allocation, not a per-step hot-path one).
ResolveScratch *acquire_resolve_scratch() noexcept {
  thread_local static ResolveScratchOwner owner;
  if (owner.scratch == nullptr) {
    owner.scratch = new (std::nothrow) ResolveScratch();
  }
  return owner.scratch;
}

} // namespace

/// Broadphase + narrow phase + constraint solve + sleep for one step:
/// collider world geometry is built serially in dense order (the only
/// simulation stage allowed to compose child transforms from the write
/// buffer), the CCD ownership/bounds snapshot is published, colliders
/// hash into a spatial grid with AABBs expanded by velocity*dt so
/// approaching pairs surface for speculative contacts, shape-pair
/// testers run per unique pair, then joints solve and bodies below the
/// energy threshold long enough are put to sleep.
bool resolve_collisions(PhysicsWorldView &world, float deltaSeconds) noexcept {
  const auto simToken = world.simulation_access_token();
  PhysicsContext &physicsCtx = world.physics_context();
  if (deltaSeconds <= 0.0F) {
    return false;
  }

  physicsCtx.collisionPairCount = 0U;
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

  ResolveScratch *const resolveScratch = acquire_resolve_scratch();
  if (resolveScratch == nullptr) {
    core::log_message(core::LogLevel::Error, "physics",
                      "resolve_collisions scratch allocation failed");
    return false;
  }

  capture_blocked_body_commands(world);

  auto &geometries = resolveScratch->geometries;
  auto &bodyOwners = resolveScratch->bodyOwners;
  auto &bodyCenters = resolveScratch->bodyCenters;
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
  }

  physicsCtx.ccdColliderCount = 0U;
  physicsCtx.ccdHasCompoundColliders = false;
  if (physicsCtx.shapeStore != nullptr) {
    PhysicsShapeStore &store = *physicsCtx.shapeStore;
    physicsCtx.ccdColliderCount = colliderCount;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      store.ccdColliderEntities[i] = entities[i];
      store.ccdColliderOwners[i] = bodyOwners[i];
      // Publish a zeroed AABB for invalid geometry: geometries[i] is
      // per-thread scratch, so its stale content would vary with whichever
      // worker ran the previous resolve.
      store.ccdColliderAabbs[i] =
          geometryValid[i] ? geometries[i].worldAabb : math::AABB{};
      if ((bodyOwners[i] != kInvalidEntity) &&
          (bodyOwners[i] != entities[i])) {
        physicsCtx.ccdHasCompoundColliders = true;
      }
    }
  }

  if (colliderCount >= 2U) {


    float cellSize = kDefaultCellSize;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      if (!geometryValid[i]) {
        continue;
      }
      const engine::math::Vec3 he =
          engine::math::aabb_half_extents(geometries[i].worldAabb);
      const float maxHe = std::max({he.x, he.y, he.z});
      if (maxHe * 2.0F > cellSize) {
        cellSize = maxHe * 2.0F;
      }
    }
    const float invCellSize = 1.0F / cellSize;

    auto &buckets = resolveScratch->buckets;
    auto &nodes = resolveScratch->nodes;
    std::size_t nodeCount = 0U;

    for (std::size_t b = 0U; b < kSpatialHashBuckets; ++b) {
      buckets[b] = kSpatialHashEmpty;
    }

    auto cell_coord = [invCellSize](float v) noexcept -> std::int32_t {
      return static_cast<std::int32_t>(std::floor(v * invCellSize));
    };

    auto hash_cell = [](std::int32_t cx, std::int32_t cy,
                        std::int32_t cz) noexcept -> std::uint32_t {
      auto u = static_cast<std::uint32_t>(cx) * 73856093U ^
               static_cast<std::uint32_t>(cy) * 19349663U ^
               static_cast<std::uint32_t>(cz) * 83492791U;
      return u % static_cast<std::uint32_t>(kSpatialHashBuckets);
    };

    auto insert_node = [&](std::uint32_t bucket,
                           std::uint32_t colIdx) noexcept {
      if (nodeCount >= kMaxNodes) {
        return;
      }
      nodes[nodeCount] = {colIdx, buckets[bucket]};
      buckets[bucket] = static_cast<std::uint32_t>(nodeCount);
      ++nodeCount;
    };

    const float speculativeDt = deltaSeconds;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      if (!geometryValid[i]) {
        continue;
      }

      const Entity bodyOwner = bodyOwners[i];
      const RigidBody *bodyI = (bodyOwner != kInvalidEntity)
                                   ? world.get_rigid_body_ptr(bodyOwner)
                                   : nullptr;
      float expandX = 0.0F;
      float expandY = 0.0F;
      float expandZ = 0.0F;
      if ((bodyI != nullptr) && (bodyI->inverseMass > 0.0F)) {
        const engine::math::Vec3 centerOffset =
            engine::math::sub(geometries[i].center, bodyCenters[i]);
        const engine::math::Vec3 pointVelocity = engine::math::add(
            bodyI->velocity,
            engine::math::cross(bodyI->angularVelocity, centerOffset));
        expandX = std::fabs(pointVelocity.x) * speculativeDt;
        expandY = std::fabs(pointVelocity.y) * speculativeDt;
        expandZ = std::fabs(pointVelocity.z) * speculativeDt;
      }

      const engine::math::AABB &bounds = geometries[i].worldAabb;
      const std::int32_t minCX = cell_coord(bounds.min.x - expandX);
      const std::int32_t maxCX = cell_coord(bounds.max.x + expandX);
      const std::int32_t minCY = cell_coord(bounds.min.y - expandY);
      const std::int32_t maxCY = cell_coord(bounds.max.y + expandY);
      const std::int32_t minCZ = cell_coord(bounds.min.z - expandZ);
      const std::int32_t maxCZ = cell_coord(bounds.max.z + expandZ);
      for (std::int32_t cx = minCX; cx <= maxCX; ++cx) {
        for (std::int32_t cy = minCY; cy <= maxCY; ++cy) {
          for (std::int32_t cz = minCZ; cz <= maxCZ; ++cz) {
            insert_node(hash_cell(cx, cy, cz), static_cast<std::uint32_t>(i));
          }
        }
      }
    }

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

      begin_generation(&physicsCtx.testedGeneration,
                       physicsCtx.testedStamps.data(),
                       physicsCtx.testedStamps.size());
      physicsCtx.testedStamps[i] = physicsCtx.testedGeneration;

      const engine::math::AABB &boundsA = geometries[i].worldAabb;
      const std::int32_t minCX = cell_coord(boundsA.min.x);
      const std::int32_t maxCX = cell_coord(boundsA.max.x);
      const std::int32_t minCY = cell_coord(boundsA.min.y);
      const std::int32_t maxCY = cell_coord(boundsA.max.y);
      const std::int32_t minCZ = cell_coord(boundsA.min.z);
      const std::int32_t maxCZ = cell_coord(boundsA.max.z);

      for (std::int32_t cx = minCX; cx <= maxCX; ++cx) {
        for (std::int32_t cy = minCY; cy <= maxCY; ++cy) {
          for (std::int32_t cz = minCZ; cz <= maxCZ; ++cz) {
            const std::uint32_t bucket = hash_cell(cx, cy, cz);
            std::uint32_t nodeIdx = buckets[bucket];
            while (nodeIdx != kSpatialHashEmpty) {
              const std::uint32_t j = nodes[nodeIdx].colliderIdx;
              nodeIdx = nodes[nodeIdx].next;

              if (physicsCtx.testedStamps[j] == physicsCtx.testedGeneration) {
                continue;
              }
              physicsCtx.testedStamps[j] = physicsCtx.testedGeneration;

              if (j <= i) {
                continue;
              }

              const Entity entityB = entities[j];
              if (!geometryValid[j]) {
                continue;
              }
              const Entity authorityEntityB =
                  (bodyOwners[j] != kInvalidEntity) ? bodyOwners[j] : entityB;
              if (world.movement_authority(authorityEntityB) ==
                  MovementAuthority::Script) {
                continue;
              }

              if ((bodyOwners[i] != kInvalidEntity) &&
                  (bodyOwners[i] == bodyOwners[j])) {
                continue;
              }

              const float bx = posX[j];
              const float by = posY[j];
              const float bz = posZ[j];

              const Collider &colliderA = colliders[i];
              const Collider &colliderB = colliders[j];

              if (((colliderA.collisionLayer & colliderB.collisionMask) ==
                   0U) ||
                  ((colliderB.collisionLayer & colliderA.collisionMask) ==
                   0U)) {
                continue;
              }

              const Entity bodyEntityA = bodyOwners[i];
              const Entity bodyEntityB = bodyOwners[j];
              RigidBody *bodyA = (bodyEntityA != kInvalidEntity)
                                     ? world.get_rigid_body_ptr(bodyEntityA)
                                     : nullptr;
              RigidBody *bodyB = (bodyEntityB != kInvalidEntity)
                                     ? world.get_rigid_body_ptr(bodyEntityB)
                                     : nullptr;
              const float invMassA =
                  (bodyA != nullptr) ? bodyA->inverseMass : kStaticInverseMass;
              const float invMassB =
                  (bodyB != nullptr) ? bodyB->inverseMass : kStaticInverseMass;
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
              const bool aIsHeightfield =
                  (shapeA == ColliderShape::Heightfield);
              const bool bIsHeightfield =
                  (shapeB == ColliderShape::Heightfield);

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
                                     requiresAffineNarrowPhase,
                                     speculativeDt};

              if (aIsHeightfield || bIsHeightfield) {
                narrow_phase_heightfield(pair);
                continue;
              }
              if (requiresAffineNarrowPhase || aIsConvex || bIsConvex) {
                narrow_phase_convex_gjk(pair);
                continue;
              }

              if (aIsCapsule && bIsCapsule) {
                narrow_phase_capsule_capsule(pair);
                continue;
              }

              if ((aIsCapsule && bIsSphere) || (aIsSphere && bIsCapsule)) {
                narrow_phase_capsule_sphere(pair);
                continue;
              }

              if ((aIsCapsule && bIsAABB) || (aIsAABB && bIsCapsule)) {
                narrow_phase_capsule_aabb(pair);
                continue;
              }

              if (aIsSphere && bIsSphere) {
                narrow_phase_sphere_sphere(pair);
                continue;
              }

              if (aIsAABB != bIsAABB) {
                narrow_phase_aabb_sphere(pair);
                continue;
              }

              narrow_phase_aabb_aabb(pair);

            }
          }
        }
      }
    }
  }

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
  }

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

void dispatch_collision_callbacks(PhysicsWorldView &world) noexcept {
  PhysicsContext &ctx = world.physics_context();
  if ((ctx.collisionDispatch != nullptr) && (ctx.collisionPairCount > 0U)) {
    ctx.collisionDispatch(ctx.collisionPairData.data(), ctx.collisionPairCount);
  }
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

void remove_joint(PhysicsWorldView &world, JointId id) noexcept {
  PhysicsContext &context = world.physics_context();
  std::size_t slotIndex = 0U;
  PhysicsJointSlot *joint = find_joint_slot(context, id, &slotIndex);
  if (joint == nullptr) {
    return;
  }

  retire_joint_slot(*joint);
  while ((context.jointCount > 0U) &&
         !context.joints[context.jointCount - 1U].active) {
    --context.jointCount;
  }
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