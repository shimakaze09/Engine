#include "engine/physics/physics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/math/aabb.h"
#include "engine/math/quat.h"
#include "engine/math/ray.h"
#include "engine/math/sphere.h"
#include "engine/math/vec3.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace engine::physics {

namespace {

constexpr float kStaticInverseMass = 0.0F;
constexpr float kDefaultCellSize = 4.0F;
constexpr std::size_t kSpatialHashBuckets = 4096U;
constexpr std::uint32_t kSpatialHashEmpty = 0xFFFFFFFFU;

constexpr std::size_t kJointSolverIterations = 4U;

constexpr float kSleepThreshold = 0.01F;
constexpr std::uint8_t kSleepFramesRequired = 60U;

float axis_overlap(float aMin, float aMax, float bMin, float bMax) noexcept {
  const float left = std::max(aMin, bMin);
  const float right = std::min(aMax, bMax);
  return right - left;
}

float sign_or_positive(float value) noexcept {
  return (value < 0.0F) ? -1.0F : 1.0F;
}

void begin_generation(std::uint32_t *generation,
                      std::uint32_t *stamps,
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

std::uint64_t make_pair_key(std::uint32_t idxA, std::uint32_t idxB) noexcept {
  const std::uint32_t lo = std::min(idxA, idxB);
  const std::uint32_t hi = std::max(idxA, idxB);
  return (static_cast<std::uint64_t>(lo) << 32U) |
         static_cast<std::uint64_t>(hi);
}

bool insert_pair_key(runtime::World::PhysicsContext &ctx,
                     std::uint64_t key) noexcept {
  const std::uint32_t generation = ctx.pairHashGeneration;
  const std::size_t bucketCount = ctx.pairHashStamps.size();
    std::size_t bucket =
      static_cast<std::size_t>((key * 11400714819323198485ULL) % bucketCount);

  for (std::size_t probe = 0U; probe < bucketCount; ++probe) {
    if (ctx.pairHashStamps[bucket] != generation) {
      ctx.pairHashStamps[bucket] = generation;
      ctx.pairHashKeys[bucket] = key;
      return true;
    }

    if (ctx.pairHashKeys[bucket] == key) {
      return false;
    }

    bucket = (bucket + 1U) % bucketCount;
  }

  return false;
}

void record_collision_pair(runtime::World &world, std::uint32_t idxA,
                           std::uint32_t idxB) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if (ctx.collisionPairCount >= runtime::World::kMaxCollisionPairs) {
    return;
  }

  if (!insert_pair_key(ctx, make_pair_key(idxA, idxB))) {
    return;
  }

  ctx.collisionPairData[ctx.collisionPairCount * 2U] = idxA;
  ctx.collisionPairData[ctx.collisionPairCount * 2U + 1U] = idxB;
  ++ctx.collisionPairCount;
}

void apply_velocity_impulse(runtime::RigidBody *bodyA,
                            runtime::RigidBody *bodyB,
                            const engine::math::Vec3 &normal, float invMassA,
                            float invMassB, float invMassSum,
                            const engine::math::Vec3 &contactOffsetA,
                            const engine::math::Vec3 &contactOffsetB,
                            float restitution, float staticFric,
                            float dynamicFric) noexcept {
  const engine::math::Vec3 velA = (bodyA != nullptr)
                                      ? bodyA->velocity
                                      : engine::math::Vec3(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 velB = (bodyB != nullptr)
                                      ? bodyB->velocity
                                      : engine::math::Vec3(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 relVel = engine::math::sub(velB, velA);
  const float relVelAlongNormal = engine::math::dot(relVel, normal);
  if (relVelAlongNormal < 0.0F) {
    const float impulseMagnitude =
        -(1.0F + restitution) * relVelAlongNormal / invMassSum;
    const engine::math::Vec3 impulseVec =
        engine::math::mul(normal, impulseMagnitude);
    if ((bodyA != nullptr) && (invMassA > 0.0F)) {
      bodyA->velocity = engine::math::sub(
          bodyA->velocity,
          engine::math::mul(normal, impulseMagnitude * invMassA));
      if (bodyA->inverseInertia > 0.0F) {
        const engine::math::Vec3 angImpulse =
            engine::math::mul(engine::math::cross(contactOffsetA, impulseVec),
                              bodyA->inverseInertia);
        bodyA->angularVelocity =
            engine::math::sub(bodyA->angularVelocity, angImpulse);
      }
    }
    if ((bodyB != nullptr) && (invMassB > 0.0F)) {
      bodyB->velocity = engine::math::add(
          bodyB->velocity,
          engine::math::mul(normal, impulseMagnitude * invMassB));
      if (bodyB->inverseInertia > 0.0F) {
        const engine::math::Vec3 angImpulse =
            engine::math::mul(engine::math::cross(contactOffsetB, impulseVec),
                              bodyB->inverseInertia);
        bodyB->angularVelocity =
            engine::math::add(bodyB->angularVelocity, angImpulse);
      }
    }

    // Tangential friction impulse.
    const engine::math::Vec3 tangentVel =
        engine::math::sub(relVel, engine::math::mul(normal, relVelAlongNormal));
    const float tangentSpeedSq = engine::math::length_sq(tangentVel);
    if (tangentSpeedSq > 1e-12F) {
      const float tangentSpeed = std::sqrt(tangentSpeedSq);
      const engine::math::Vec3 tangent =
          engine::math::div(tangentVel, tangentSpeed);
      float frictionImpulse = -tangentSpeed / invMassSum;
      if (std::fabs(frictionImpulse) < impulseMagnitude * staticFric) {
        // Static friction: apply exact counter-impulse.
      } else {
        frictionImpulse =
            sign_or_positive(frictionImpulse) * impulseMagnitude * dynamicFric;
      }
      if ((bodyA != nullptr) && (invMassA > 0.0F)) {
        bodyA->velocity = engine::math::sub(
            bodyA->velocity,
            engine::math::mul(tangent, frictionImpulse * invMassA));
      }
      if ((bodyB != nullptr) && (invMassB > 0.0F)) {
        bodyB->velocity = engine::math::add(
            bodyB->velocity,
            engine::math::mul(tangent, frictionImpulse * invMassB));
      }
    }
  }
}

} // namespace

void solve_joints(runtime::World &world) noexcept;

bool step_physics(runtime::World &world, float deltaSeconds) noexcept {
  return step_physics_range(world, 0U, world.transform_count(), deltaSeconds);
}

bool step_physics_range(runtime::World &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept {
  const runtime::World::PhysicsContext &physicsCtx = world.physics_context();
  const runtime::Entity *entities = nullptr;
  const runtime::Transform *readTransforms = nullptr;
  runtime::Transform *writeTransforms = nullptr;

  if (!world.get_transform_update_range(startIndex, count, &entities,
                                        &readTransforms, &writeTransforms)) {
    return false;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = entities[i];
    if (world.movement_authority(entity) ==
        runtime::MovementAuthority::Script) {
      writeTransforms[i] = readTransforms[i];
      continue;
    }

    runtime::RigidBody *body = world.get_rigid_body_ptr(entity);
    runtime::Transform updated = readTransforms[i];

    // Skip sleeping bodies.
    if ((body != nullptr) && body->sleeping) {
      writeTransforms[i] = readTransforms[i];
      continue;
    }

    if ((body != nullptr) && (body->inverseMass > 0.0F)) {
      const engine::math::Vec3 totalAccel =
          engine::math::add(body->acceleration, physicsCtx.gravity);
      body->velocity = engine::math::add(
          body->velocity, engine::math::mul(totalAccel, deltaSeconds));

      const engine::math::Vec3 displacement =
          engine::math::mul(body->velocity, deltaSeconds);
      const float speed = engine::math::length(body->velocity);
      const float travelDist = speed * deltaSeconds;

      // CCD: if travel distance exceeds half the smallest collider extent,
      // raycast forward to prevent tunneling.
      const runtime::Collider *col = world.get_collider_ptr(entity);
      const float minHalf =
          (col != nullptr) ? std::min({col->halfExtents.x, col->halfExtents.y,
                                       col->halfExtents.z})
                           : 0.0F;

      bool clamped = false;
      if ((col != nullptr) && (travelDist > minHalf) && (speed > 1e-6F)) {
        const engine::math::Vec3 dir = engine::math::div(body->velocity, speed);
        runtime::PhysicsRaycastHit hit{};
        if (raycast(world, readTransforms[i].position, dir, travelDist, &hit,
                    entity)) {
          // Clamp position to just before the hit surface.
          const float safeT = std::max(0.0F, hit.distance - minHalf);
          updated.position = engine::math::add(readTransforms[i].position,
                                               engine::math::mul(dir, safeT));

          // Reflect velocity off the hit normal.
          const float vn = engine::math::dot(body->velocity, hit.normal);
          if (vn < 0.0F) {
            body->velocity = engine::math::sub(
                body->velocity, engine::math::mul(hit.normal, 2.0F * vn));
          }
          clamped = true;
        }
      }

      if (!clamped) {
        updated.position = engine::math::add(updated.position, displacement);
      }
    }

    // Angular velocity integration (independent of linear mass).
    if ((body != nullptr) && (body->inverseInertia > 0.0F)) {
      const float angSpeedSq = engine::math::length_sq(body->angularVelocity);
      if (angSpeedSq > 1e-12F) {
        const float angSpeed = std::sqrt(angSpeedSq);
        const float angle = angSpeed * deltaSeconds;
        const engine::math::Vec3 axis =
            engine::math::div(body->angularVelocity, angSpeed);
        const engine::math::Quat deltaRot =
            engine::math::from_axis_angle(axis, angle);
        updated.rotation = engine::math::normalize(
            engine::math::mul(deltaRot, updated.rotation));
      }
    }

    writeTransforms[i] = updated;
  }

  return true;
}

bool resolve_collisions(runtime::World &world) noexcept {
  runtime::World::PhysicsContext &physicsCtx = world.physics_context();
  physicsCtx.collisionPairCount = 0U;
  begin_generation(&physicsCtx.pairHashGeneration, physicsCtx.pairHashStamps.data(),
                   physicsCtx.pairHashStamps.size());

  const std::size_t colliderCount = world.collider_count();

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if ((colliderCount > 0U) &&
      !world.get_collider_range(0U, colliderCount, &entities, &colliders)) {
    return false;
  }

  if (colliderCount >= 2U) {

    // ---- Broadphase: spatial hash grid
    // ----------------------------------------

    // Per-collider cached position.
    thread_local static std::array<float, runtime::World::kMaxColliders> posX{},
        posY{}, posZ{};

    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const runtime::Transform *t = world.get_transform_write_ptr(entities[i]);
      if (t != nullptr) {
        posX[i] = t->position.x;
        posY[i] = t->position.y;
        posZ[i] = t->position.z;
      } else {
        posX[i] = 0.0F;
        posY[i] = 0.0F;
        posZ[i] = 0.0F;
      }
    }

    // Compute cell size: max of kDefaultCellSize and 2× largest half-extent.
    float cellSize = kDefaultCellSize;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const engine::math::Vec3 &he = colliders[i].halfExtents;
      const float maxHe = std::max({he.x, he.y, he.z});
      if (maxHe * 2.0F > cellSize) {
        cellSize = maxHe * 2.0F;
      }
    }
    const float invCellSize = 1.0F / cellSize;

    // Spatial hash: bucket heads + linked-list nodes.
    struct SpatialNode {
      std::uint32_t colliderIdx;
      std::uint32_t next;
    };

    // Max entries: each collider may touch up to 8 cells (corners of its AABB).
    constexpr std::size_t kMaxNodes = runtime::World::kMaxColliders * 8U;
    thread_local static std::array<std::uint32_t, kSpatialHashBuckets>
        buckets{};
    thread_local static std::array<SpatialNode, kMaxNodes> nodes{};
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

    // Insert each collider into all cells its AABB overlaps.
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const engine::math::Vec3 &he = colliders[i].halfExtents;
      const std::int32_t minCX = cell_coord(posX[i] - he.x);
      const std::int32_t maxCX = cell_coord(posX[i] + he.x);
      const std::int32_t minCY = cell_coord(posY[i] - he.y);
      const std::int32_t maxCY = cell_coord(posY[i] + he.y);
      const std::int32_t minCZ = cell_coord(posZ[i] - he.z);
      const std::int32_t maxCZ = cell_coord(posZ[i] + he.z);
      for (std::int32_t cx = minCX; cx <= maxCX; ++cx) {
        for (std::int32_t cy = minCY; cy <= maxCY; ++cy) {
          for (std::int32_t cz = minCZ; cz <= maxCZ; ++cz) {
            insert_node(hash_cell(cx, cy, cz), static_cast<std::uint32_t>(i));
          }
        }
      }
    }

    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const runtime::Entity entityA = entities[i];
      if (world.movement_authority(entityA) ==
          runtime::MovementAuthority::Script) {
        continue;
      }

      const runtime::Transform *transformA =
          world.get_transform_write_ptr(entityA);
      if (transformA == nullptr) {
        continue;
      }

      const float ax = posX[i];
      const float ay = posY[i];
      const float az = posZ[i];

      begin_generation(&physicsCtx.testedGeneration,
                       physicsCtx.testedStamps.data(),
                       physicsCtx.testedStamps.size());
      physicsCtx.testedStamps[i] = physicsCtx.testedGeneration;

      const engine::math::Vec3 &heA = colliders[i].halfExtents;
      const std::int32_t minCX = cell_coord(ax - heA.x);
      const std::int32_t maxCX = cell_coord(ax + heA.x);
      const std::int32_t minCY = cell_coord(ay - heA.y);
      const std::int32_t maxCY = cell_coord(ay + heA.y);
      const std::int32_t minCZ = cell_coord(az - heA.z);
      const std::int32_t maxCZ = cell_coord(az + heA.z);

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

              // Only process pair (i, j) where i < j to avoid
              // double-processing.
              if (j <= i) {
                continue;
              }

              const runtime::Entity entityB = entities[j];
              if (world.movement_authority(entityB) ==
                  runtime::MovementAuthority::Script) {
                continue;
              }

              const runtime::Transform *transformB =
                  world.get_transform_write_ptr(entityB);
              if (transformB == nullptr) {
                continue;
              }

              const float bx = posX[j];
              const float by = posY[j];
              const float bz = posZ[j];

              const runtime::Collider &colliderA = colliders[i];
              const runtime::Collider &colliderB = colliders[j];

              runtime::RigidBody *bodyA = world.get_rigid_body_ptr(entityA);
              runtime::RigidBody *bodyB = world.get_rigid_body_ptr(entityB);
              const float invMassA =
                  (bodyA != nullptr) ? bodyA->inverseMass : kStaticInverseMass;
              const float invMassB =
                  (bodyB != nullptr) ? bodyB->inverseMass : kStaticInverseMass;
              const float invMassSum = invMassA + invMassB;

              const bool aIsAABB =
                  (colliderA.shape == runtime::ColliderShape::AABB);
              const bool bIsAABB =
                  (colliderB.shape == runtime::ColliderShape::AABB);

              // -----------------------------------------------------------------------
              // Sphere vs Sphere
              // -----------------------------------------------------------------------
              if (!aIsAABB && !bIsAABB) {
                const float rA = colliderA.halfExtents.x;
                const float rB = colliderB.halfExtents.x;
                const float dx = bx - ax;
                const float dy = by - ay;
                const float dz = bz - az;
                const float dist2 = dx * dx + dy * dy + dz * dz;
                const float sumR = rA + rB;
                if (dist2 >= sumR * sumR) {
                  continue;
                }

                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  if ((bodyA != nullptr) && bodyA->sleeping &&
                      (vB2 > kSleepThreshold)) {
                    bodyA->sleeping = false;
                    bodyA->sleepFrameCount = 0U;
                  }
                  if ((bodyB != nullptr) && bodyB->sleeping &&
                      (vA2 > kSleepThreshold)) {
                    bodyB->sleeping = false;
                    bodyB->sleepFrameCount = 0U;
                  }
                }

                if (invMassSum <= 0.0F) {
                  continue;
                }

                const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                const float nx = dx / dist;
                const float ny = dy / dist;
                const float nz = dz / dist;
                const float overlap = sumR - dist;
                const float moveA = overlap * (invMassA / invMassSum);
                const float moveB = overlap * (invMassB / invMassSum);

                runtime::Transform *mutableA =
                    world.get_transform_write_ptr(entityA);
                runtime::Transform *mutableB =
                    world.get_transform_write_ptr(entityB);
                if ((mutableA == nullptr) || (mutableB == nullptr)) {
                  continue;
                }
                mutableA->position.x -= nx * moveA;
                mutableA->position.y -= ny * moveA;
                mutableA->position.z -= nz * moveA;
                mutableB->position.x += nx * moveB;
                mutableB->position.y += ny * moveB;
                mutableB->position.z += nz * moveB;

                const engine::math::Vec3 contactNormal(nx, ny, nz);
                const engine::math::Vec3 contactPt = engine::math::mul(
                    engine::math::add(mutableA->position, mutableB->position),
                    0.5F);
                const float combinedRest =
                    std::max(colliderA.restitution, colliderB.restitution);
                const float combinedStaticFric = std::sqrt(
                    colliderA.staticFriction * colliderB.staticFriction);
                const float combinedDynFric = std::sqrt(
                    colliderA.dynamicFriction * colliderB.dynamicFriction);
                apply_velocity_impulse(
                    bodyA, bodyB, contactNormal, invMassA, invMassB, invMassSum,
                    engine::math::sub(contactPt, mutableA->position),
                    engine::math::sub(contactPt, mutableB->position),
                    combinedRest, combinedStaticFric, combinedDynFric);
                continue;
              }

              // -----------------------------------------------------------------------
              // AABB vs Sphere (handles both orderings)
              // -----------------------------------------------------------------------
              if (aIsAABB != bIsAABB) {
                const bool aIsBox = aIsAABB;
                const float boxX = aIsBox ? ax : bx;
                const float boxY = aIsBox ? ay : by;
                const float boxZ = aIsBox ? az : bz;
                const float sphX = aIsBox ? bx : ax;
                const float sphY = aIsBox ? by : ay;
                const float sphZ = aIsBox ? bz : az;
                const runtime::Collider &boxCol =
                    aIsBox ? colliderA : colliderB;
                const float radius =
                    (aIsBox ? colliderB : colliderA).halfExtents.x;

                const float cpx =
                    std::max(boxX - boxCol.halfExtents.x,
                             std::min(sphX, boxX + boxCol.halfExtents.x));
                const float cpy =
                    std::max(boxY - boxCol.halfExtents.y,
                             std::min(sphY, boxY + boxCol.halfExtents.y));
                const float cpz =
                    std::max(boxZ - boxCol.halfExtents.z,
                             std::min(sphZ, boxZ + boxCol.halfExtents.z));

                const float dx = sphX - cpx;
                const float dy = sphY - cpy;
                const float dz = sphZ - cpz;
                const float dist2 = dx * dx + dy * dy + dz * dz;
                if (dist2 >= radius * radius) {
                  continue;
                }

                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  if ((bodyA != nullptr) && bodyA->sleeping &&
                      (vB2 > kSleepThreshold)) {
                    bodyA->sleeping = false;
                    bodyA->sleepFrameCount = 0U;
                  }
                  if ((bodyB != nullptr) && bodyB->sleeping &&
                      (vA2 > kSleepThreshold)) {
                    bodyB->sleeping = false;
                    bodyB->sleepFrameCount = 0U;
                  }
                }

                if (invMassSum <= 0.0F) {
                  continue;
                }

                const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                float nx = (dist2 > 0.0F) ? (dx / dist) : 0.0F;
                float ny = (dist2 > 0.0F) ? (dy / dist) : 1.0F;
                float nz = (dist2 > 0.0F) ? (dz / dist) : 0.0F;
                const float overlap = radius - dist;

                // Normal points from box toward sphere; flip if A is the
                // sphere.
                if (!aIsBox) {
                  nx = -nx;
                  ny = -ny;
                  nz = -nz;
                }

                const float moveA = overlap * (invMassA / invMassSum);
                const float moveB = overlap * (invMassB / invMassSum);

                runtime::Transform *mutableA =
                    world.get_transform_write_ptr(entityA);
                runtime::Transform *mutableB =
                    world.get_transform_write_ptr(entityB);
                if ((mutableA == nullptr) || (mutableB == nullptr)) {
                  continue;
                }
                mutableA->position.x -= nx * moveA;
                mutableA->position.y -= ny * moveA;
                mutableA->position.z -= nz * moveA;
                mutableB->position.x += nx * moveB;
                mutableB->position.y += ny * moveB;
                mutableB->position.z += nz * moveB;

                const engine::math::Vec3 aabbSphNormal(nx, ny, nz);
                const engine::math::Vec3 closestPt(cpx, cpy, cpz);
                const float combinedRest =
                    std::max(colliderA.restitution, colliderB.restitution);
                const float combinedStaticFric = std::sqrt(
                    colliderA.staticFriction * colliderB.staticFriction);
                const float combinedDynFric = std::sqrt(
                    colliderA.dynamicFriction * colliderB.dynamicFriction);
                apply_velocity_impulse(
                    bodyA, bodyB, aabbSphNormal, invMassA, invMassB, invMassSum,
                    engine::math::sub(closestPt, mutableA->position),
                    engine::math::sub(closestPt, mutableB->position),
                    combinedRest, combinedStaticFric, combinedDynFric);
                continue;
              }

              // -----------------------------------------------------------------------
              // AABB vs AABB
              // -----------------------------------------------------------------------
              const float overlapX = axis_overlap(
                  ax - colliderA.halfExtents.x, ax + colliderA.halfExtents.x,
                  bx - colliderB.halfExtents.x, bx + colliderB.halfExtents.x);
              if (overlapX <= 0.0F) {
                continue;
              }

              const float overlapY = axis_overlap(
                  ay - colliderA.halfExtents.y, ay + colliderA.halfExtents.y,
                  by - colliderB.halfExtents.y, by + colliderB.halfExtents.y);
              if (overlapY <= 0.0F) {
                continue;
              }

              const float overlapZ = axis_overlap(
                  az - colliderA.halfExtents.z, az + colliderA.halfExtents.z,
                  bz - colliderB.halfExtents.z, bz + colliderB.halfExtents.z);
              if (overlapZ <= 0.0F) {
                continue;
              }

              record_collision_pair(world, entityA.index, entityB.index);
              {
                const float vA2 = (bodyA != nullptr)
                                      ? engine::math::length_sq(bodyA->velocity)
                                      : 0.0F;
                const float vB2 = (bodyB != nullptr)
                                      ? engine::math::length_sq(bodyB->velocity)
                                      : 0.0F;
                if ((bodyA != nullptr) && bodyA->sleeping &&
                    (vB2 > kSleepThreshold)) {
                  bodyA->sleeping = false;
                  bodyA->sleepFrameCount = 0U;
                }
                if ((bodyB != nullptr) && bodyB->sleeping &&
                    (vA2 > kSleepThreshold)) {
                  bodyB->sleeping = false;
                  bodyB->sleepFrameCount = 0U;
                }
              }

              if (invMassSum <= 0.0F) {
                continue;
              }

              float pushAmount = overlapX;
              float pushX = sign_or_positive(bx - ax);
              float pushY = 0.0F;
              float pushZ = 0.0F;

              if (overlapY < pushAmount) {
                pushAmount = overlapY;
                pushX = 0.0F;
                pushY = sign_or_positive(by - ay);
                pushZ = 0.0F;
              }

              if (overlapZ < pushAmount) {
                pushAmount = overlapZ;
                pushX = 0.0F;
                pushY = 0.0F;
                pushZ = sign_or_positive(bz - az);
              }

              const float moveA = pushAmount * (invMassA / invMassSum);
              const float moveB = pushAmount * (invMassB / invMassSum);

              runtime::Transform *mutableA =
                  world.get_transform_write_ptr(entityA);
              runtime::Transform *mutableB =
                  world.get_transform_write_ptr(entityB);
              if ((mutableA == nullptr) || (mutableB == nullptr)) {
                continue;
              }

              mutableA->position.x -= pushX * moveA;
              mutableA->position.y -= pushY * moveA;
              mutableA->position.z -= pushZ * moveA;
              mutableB->position.x += pushX * moveB;
              mutableB->position.y += pushY * moveB;
              mutableB->position.z += pushZ * moveB;

              const engine::math::Vec3 aabbNormal(pushX, pushY, pushZ);
              const engine::math::Vec3 midPt = engine::math::mul(
                  engine::math::add(mutableA->position, mutableB->position),
                  0.5F);
              const float combinedRest =
                  std::max(colliderA.restitution, colliderB.restitution);
              const float combinedStaticFric = std::sqrt(
                  colliderA.staticFriction * colliderB.staticFriction);
              const float combinedDynFric = std::sqrt(
                  colliderA.dynamicFriction * colliderB.dynamicFriction);
              apply_velocity_impulse(
                  bodyA, bodyB, aabbNormal, invMassA, invMassB, invMassSum,
                  engine::math::sub(midPt, mutableA->position),
                  engine::math::sub(midPt, mutableB->position), combinedRest,
                  combinedStaticFric, combinedDynFric);

            } // while nodeIdx
          } // for cz
        } // for cy
      } // for cx
    } // for i
  } // if (colliderCount >= 2U)

  solve_joints(world);

  // Sleep check: after all collision responses and joint solving,
  // examine each rigid body. If velocity is below threshold for enough
  // consecutive frames, put it to sleep.
  for (std::size_t i = 0U; i < colliderCount; ++i) {
    runtime::RigidBody *body = world.get_rigid_body_ptr(entities[i]);
    if ((body == nullptr) || (body->inverseMass <= 0.0F) || body->sleeping) {
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

  return true;
}

void set_gravity(runtime::World &world, float x, float y, float z) noexcept {
  world.physics_context().gravity = engine::math::Vec3(x, y, z);
}

engine::math::Vec3 get_gravity(const runtime::World &world) noexcept {
  return world.physics_context().gravity;
}

void set_collision_dispatch(runtime::World &world,
                            CollisionDispatchFn fn) noexcept {
  world.physics_context().collisionDispatch = fn;
}

void dispatch_collision_callbacks(runtime::World &world) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if ((ctx.collisionDispatch != nullptr) && (ctx.collisionPairCount > 0U)) {
    ctx.collisionDispatch(ctx.collisionPairData.data(), ctx.collisionPairCount);
  }
  ctx.collisionPairCount = 0U;
}

namespace {

engine::math::Vec3 aabb_hit_normal(const engine::math::Vec3 &hitPoint,
                                   const engine::math::Vec3 &center,
                                   const engine::math::Vec3 &halfExt) noexcept {
  const float dx = (hitPoint.x - center.x) / halfExt.x;
  const float dy = (hitPoint.y - center.y) / halfExt.y;
  const float dz = (hitPoint.z - center.z) / halfExt.z;
  const float ax = std::fabs(dx);
  const float ay = std::fabs(dy);
  const float az = std::fabs(dz);
  // If all deviations are zero (exact center hit), return zero normal.
  if ((ax < 1e-6F) && (ay < 1e-6F) && (az < 1e-6F)) {
    return engine::math::Vec3(0.0F, 0.0F, 0.0F);
  }
  if ((ax >= ay) && (ax >= az)) {
    return engine::math::Vec3(sign_or_positive(dx), 0.0F, 0.0F);
  }
  if (ay >= az) {
    return engine::math::Vec3(0.0F, sign_or_positive(dy), 0.0F);
  }
  return engine::math::Vec3(0.0F, 0.0F, sign_or_positive(dz));
}

} // namespace

bool raycast(const runtime::World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             runtime::PhysicsRaycastHit *outHit,
             runtime::Entity skipEntity) noexcept {
  // Validate direction vector to prevent NaN from normalization.
  if (math::length_sq(direction) < 1e-12F) {
    return false;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return false;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }

  const math::Ray ray{origin, direction};
  bool hit = false;
  float bestT = maxDistance;

  for (std::size_t i = 0U; i < count; ++i) {
    if (entities[i] == skipEntity) {
      continue;
    }
    runtime::Transform transform{};
    if (!world.get_transform(entities[i], &transform)) {
      continue;
    }
    const runtime::Collider &col = colliders[i];
    float t = 0.0F;

    if (col.shape == runtime::ColliderShape::Sphere) {
      const math::Sphere sphere{transform.position, col.halfExtents.x};
      if (!math::ray_intersects_sphere(ray, sphere, &t)) {
        continue;
      }
    } else {
      const math::AABB box = math::aabb_from_center_half_extents(
          transform.position, col.halfExtents);
      if (!math::ray_intersects_aabb(ray, box, &t)) {
        continue;
      }
    }

    if ((t >= 0.0F) && (t <= bestT)) {
      bestT = t;
      hit = true;
      if (outHit != nullptr) {
        outHit->entity = entities[i];
        outHit->distance = t;
        outHit->point = math::add(origin, math::mul(direction, t));
        if (col.shape == runtime::ColliderShape::Sphere) {
          outHit->normal =
              math::normalize(math::sub(outHit->point, transform.position));
        } else {
          outHit->normal = aabb_hit_normal(outHit->point, transform.position,
                                           col.halfExtents);
        }
      }
    }
  }

  return hit;
}

std::size_t raycast_all(const runtime::World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        runtime::PhysicsRaycastHit *outHits,
                        std::size_t maxHits) noexcept {
  // Validate direction vector to prevent NaN from normalization.
  if (math::length_sq(direction) < 1e-12F) {
    return 0U;
  }

  if ((outHits == nullptr) || (maxHits == 0U)) {
    return 0U;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return 0U;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return 0U;
  }

  const math::Ray ray{origin, direction};
  std::size_t hitCount = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    runtime::Transform transform{};
    if (!world.get_transform(entities[i], &transform)) {
      continue;
    }
    const runtime::Collider &col = colliders[i];
    float t = 0.0F;

    if (col.shape == runtime::ColliderShape::Sphere) {
      const math::Sphere sphere{transform.position, col.halfExtents.x};
      if (!math::ray_intersects_sphere(ray, sphere, &t)) {
        continue;
      }
    } else {
      const math::AABB box = math::aabb_from_center_half_extents(
          transform.position, col.halfExtents);
      if (!math::ray_intersects_aabb(ray, box, &t)) {
        continue;
      }
    }

    if ((t >= 0.0F) && (t <= maxDistance)) {
      if (hitCount < maxHits) {
        runtime::PhysicsRaycastHit &rh = outHits[hitCount];
        rh.entity = entities[i];
        rh.distance = t;
        rh.point = math::add(origin, math::mul(direction, t));
        if (col.shape == runtime::ColliderShape::Sphere) {
          rh.normal = math::normalize(math::sub(rh.point, transform.position));
        } else {
          rh.normal =
              aabb_hit_normal(rh.point, transform.position, col.halfExtents);
        }
        ++hitCount;
      }
    }
  }

  // Sort hits by distance.
  std::sort(outHits, outHits + hitCount,
            [](const runtime::PhysicsRaycastHit &a,
               const runtime::PhysicsRaycastHit &b) noexcept {
              return a.distance < b.distance;
            });

  return hitCount;
}

JointId add_distance_joint(runtime::World &world, runtime::Entity entityA,
                           runtime::Entity entityB,
                           float distance) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  for (std::size_t i = 0U; i < runtime::World::kMaxPhysicsJoints; ++i) {
    if (!ctx.joints[i].active) {
      ctx.joints[i].entityA = entityA;
      ctx.joints[i].entityB = entityB;
      ctx.joints[i].distance = distance;
      ctx.joints[i].active = true;
      if (i >= ctx.jointCount) {
        ctx.jointCount = i + 1U;
      }
      return static_cast<JointId>(i);
    }
  }
  return kInvalidJointId;
}

void remove_joint(runtime::World &world, JointId id) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if (id >= runtime::World::kMaxPhysicsJoints) {
    return;
  }
  ctx.joints[id].active = false;
  // Shrink high-water mark.
  while ((ctx.jointCount > 0U) && !ctx.joints[ctx.jointCount - 1U].active) {
    --ctx.jointCount;
  }
}

void solve_joints(runtime::World &world) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if (ctx.jointCount == 0U) {
    return;
  }

  for (std::size_t iter = 0U; iter < kJointSolverIterations; ++iter) {
    for (std::size_t i = 0U; i < ctx.jointCount; ++i) {
      if (!ctx.joints[i].active) {
        continue;
      }

      runtime::Transform *tA =
          world.get_transform_write_ptr(ctx.joints[i].entityA);
      runtime::Transform *tB =
          world.get_transform_write_ptr(ctx.joints[i].entityB);
      if ((tA == nullptr) || (tB == nullptr)) {
        continue;
      }

      runtime::RigidBody *bodyA =
          world.get_rigid_body_ptr(ctx.joints[i].entityA);
      runtime::RigidBody *bodyB =
          world.get_rigid_body_ptr(ctx.joints[i].entityB);
      const float invMassA = (bodyA != nullptr) ? bodyA->inverseMass : 0.0F;
      const float invMassB = (bodyB != nullptr) ? bodyB->inverseMass : 0.0F;
      const float invMassSum = invMassA + invMassB;
      if (invMassSum <= 0.0F) {
        continue;
      }

      const math::Vec3 delta = math::sub(tB->position, tA->position);
      const float currentDist = math::length(delta);
      if (currentDist < 1e-8F) {
        continue;
      }
      const float error = currentDist - ctx.joints[i].distance;
      const math::Vec3 dir = math::div(delta, currentDist);
      const math::Vec3 correction = math::mul(dir, error);

      tA->position =
          math::add(tA->position, math::mul(correction, invMassA / invMassSum));
      tB->position =
          math::sub(tB->position, math::mul(correction, invMassB / invMassSum));
    }
  }

  // Velocity correction: derive velocity from position change is implicit
  // since we modify write-buffer positions directly.
}

void wake_body(runtime::World &world, runtime::Entity entity) noexcept {
  runtime::RigidBody *body = world.get_rigid_body_ptr(entity);
  if (body != nullptr) {
    body->sleeping = false;
    body->sleepFrameCount = 0U;
  }
}

bool is_sleeping(const runtime::World &world, runtime::Entity entity) noexcept {
  runtime::RigidBody rb{};
  if (!world.get_rigid_body(entity, &rb)) {
    return false;
  }
  return rb.sleeping;
}

} // namespace engine::physics
