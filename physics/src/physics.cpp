#include "engine/physics/physics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::physics {

namespace {

constexpr float kStaticInverseMass = 0.0F;
constexpr std::size_t kMaxCollisionPairs = 256U;

engine::math::Vec3 g_gravity(0.0F, -9.8F, 0.0F);

// Packed collision pairs: [entityIndexA0, entityIndexB0, ...]
std::array<std::uint32_t, kMaxCollisionPairs * 2U> g_collisionPairData{};
std::size_t g_collisionPairCount = 0U;
CollisionDispatchFn g_collisionDispatch = nullptr;

float axis_overlap(float aMin, float aMax, float bMin, float bMax) noexcept {
  const float left = std::max(aMin, bMin);
  const float right = std::min(aMax, bMax);
  return right - left;
}

float sign_or_positive(float value) noexcept {
  return (value < 0.0F) ? -1.0F : 1.0F;
}

void record_collision_pair(std::uint32_t idxA, std::uint32_t idxB) noexcept {
  for (std::size_t k = 0U; k < g_collisionPairCount; ++k) {
    const std::uint32_t pa = g_collisionPairData[k * 2U];
    const std::uint32_t pb = g_collisionPairData[k * 2U + 1U];
    if ((pa == idxA && pb == idxB) || (pa == idxB && pb == idxA)) {
      return;
    }
  }
  if (g_collisionPairCount < kMaxCollisionPairs) {
    g_collisionPairData[g_collisionPairCount * 2U] = idxA;
    g_collisionPairData[g_collisionPairCount * 2U + 1U] = idxB;
    ++g_collisionPairCount;
  }
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

bool step_physics(runtime::World &world, float deltaSeconds) noexcept {
  return step_physics_range(world, 0U, world.transform_count(), deltaSeconds);
}

bool step_physics_range(runtime::World &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept {
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

    if ((body != nullptr) && (body->inverseMass > 0.0F)) {
      const engine::math::Vec3 totalAccel =
          engine::math::add(body->acceleration, g_gravity);
      body->velocity = engine::math::add(
          body->velocity, engine::math::mul(totalAccel, deltaSeconds));
      updated.position = engine::math::add(
          updated.position, engine::math::mul(body->velocity, deltaSeconds));
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
  const std::size_t colliderCount = world.collider_count();
  if (colliderCount < 2U) {
    return true;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, colliderCount, &entities, &colliders)) {
    return false;
  }

  // Broadphase: sort-and-sweep on the X axis.
  thread_local static std::array<std::uint32_t, runtime::World::kMaxColliders>
      sortedIdx{};
  thread_local static std::array<float, runtime::World::kMaxColliders>
      sweepMinX{};
  thread_local static std::array<float, runtime::World::kMaxColliders>
      sweepMaxX{};

  for (std::size_t i = 0U; i < colliderCount; ++i) {
    sortedIdx[i] = static_cast<std::uint32_t>(i);
    const runtime::Transform *t = world.get_transform_write_ptr(entities[i]);
    if (t != nullptr) {
      sweepMinX[i] = t->position.x - colliders[i].halfExtents.x;
      sweepMaxX[i] = t->position.x + colliders[i].halfExtents.x;
    } else {
      sweepMinX[i] = 0.0F;
      sweepMaxX[i] = 0.0F;
    }
  }

  const float *pMin = sweepMinX.data();
  std::sort(sortedIdx.data(), sortedIdx.data() + colliderCount,
            [pMin](std::uint32_t a, std::uint32_t b) noexcept {
              return pMin[a] < pMin[b];
            });

  for (std::size_t si = 0U; si < colliderCount; ++si) {
    const std::uint32_t i = sortedIdx[si];
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

    const float ax = transformA->position.x;
    const float ay = transformA->position.y;
    const float az = transformA->position.z;
    const float maxXi = sweepMaxX[i];

    for (std::size_t sj = si + 1U; sj < colliderCount; ++sj) {
      const std::uint32_t j = sortedIdx[sj];

      // Broadphase early exit.
      if (sweepMinX[j] > maxXi) {
        break;
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

      const float bx = transformB->position.x;
      const float by = transformB->position.y;
      const float bz = transformB->position.z;

      const runtime::Collider &colliderA = colliders[i];
      const runtime::Collider &colliderB = colliders[j];

      runtime::RigidBody *bodyA = world.get_rigid_body_ptr(entityA);
      runtime::RigidBody *bodyB = world.get_rigid_body_ptr(entityB);
      const float invMassA =
          (bodyA != nullptr) ? bodyA->inverseMass : kStaticInverseMass;
      const float invMassB =
          (bodyB != nullptr) ? bodyB->inverseMass : kStaticInverseMass;
      const float invMassSum = invMassA + invMassB;

      const bool aIsAABB = (colliderA.shape == runtime::ColliderShape::AABB);
      const bool bIsAABB = (colliderB.shape == runtime::ColliderShape::AABB);

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

        record_collision_pair(entityA.index, entityB.index);

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

        runtime::Transform *mutableA = world.get_transform_write_ptr(entityA);
        runtime::Transform *mutableB = world.get_transform_write_ptr(entityB);
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
            engine::math::add(mutableA->position, mutableB->position), 0.5F);
        const float combinedRest =
            std::max(colliderA.restitution, colliderB.restitution);
        const float combinedStaticFric =
            std::sqrt(colliderA.staticFriction * colliderB.staticFriction);
        const float combinedDynFric =
            std::sqrt(colliderA.dynamicFriction * colliderB.dynamicFriction);
        apply_velocity_impulse(
            bodyA, bodyB, contactNormal, invMassA, invMassB, invMassSum,
            engine::math::sub(contactPt, mutableA->position),
            engine::math::sub(contactPt, mutableB->position), combinedRest,
            combinedStaticFric, combinedDynFric);
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
        const runtime::Collider &boxCol = aIsBox ? colliderA : colliderB;
        const float radius = (aIsBox ? colliderB : colliderA).halfExtents.x;

        const float cpx = std::max(boxX - boxCol.halfExtents.x,
                                   std::min(sphX, boxX + boxCol.halfExtents.x));
        const float cpy = std::max(boxY - boxCol.halfExtents.y,
                                   std::min(sphY, boxY + boxCol.halfExtents.y));
        const float cpz = std::max(boxZ - boxCol.halfExtents.z,
                                   std::min(sphZ, boxZ + boxCol.halfExtents.z));

        const float dx = sphX - cpx;
        const float dy = sphY - cpy;
        const float dz = sphZ - cpz;
        const float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 >= radius * radius) {
          continue;
        }

        record_collision_pair(entityA.index, entityB.index);

        if (invMassSum <= 0.0F) {
          continue;
        }

        const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
        float nx = (dist2 > 0.0F) ? (dx / dist) : 0.0F;
        float ny = (dist2 > 0.0F) ? (dy / dist) : 1.0F;
        float nz = (dist2 > 0.0F) ? (dz / dist) : 0.0F;
        const float overlap = radius - dist;

        // Normal points from box toward sphere; flip if A is the sphere.
        if (!aIsBox) {
          nx = -nx;
          ny = -ny;
          nz = -nz;
        }

        const float moveA = overlap * (invMassA / invMassSum);
        const float moveB = overlap * (invMassB / invMassSum);

        runtime::Transform *mutableA = world.get_transform_write_ptr(entityA);
        runtime::Transform *mutableB = world.get_transform_write_ptr(entityB);
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
        const float combinedStaticFric =
            std::sqrt(colliderA.staticFriction * colliderB.staticFriction);
        const float combinedDynFric =
            std::sqrt(colliderA.dynamicFriction * colliderB.dynamicFriction);
        apply_velocity_impulse(
            bodyA, bodyB, aabbSphNormal, invMassA, invMassB, invMassSum,
            engine::math::sub(closestPt, mutableA->position),
            engine::math::sub(closestPt, mutableB->position), combinedRest,
            combinedStaticFric, combinedDynFric);
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

      record_collision_pair(entityA.index, entityB.index);

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

      runtime::Transform *mutableA = world.get_transform_write_ptr(entityA);
      runtime::Transform *mutableB = world.get_transform_write_ptr(entityB);
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
          engine::math::add(mutableA->position, mutableB->position), 0.5F);
      const float combinedRest =
          std::max(colliderA.restitution, colliderB.restitution);
      const float combinedStaticFric =
          std::sqrt(colliderA.staticFriction * colliderB.staticFriction);
      const float combinedDynFric =
          std::sqrt(colliderA.dynamicFriction * colliderB.dynamicFriction);
      apply_velocity_impulse(bodyA, bodyB, aabbNormal, invMassA, invMassB,
                             invMassSum,
                             engine::math::sub(midPt, mutableA->position),
                             engine::math::sub(midPt, mutableB->position),
                             combinedRest, combinedStaticFric, combinedDynFric);
    }
  }

  return true;
}

void set_gravity(float x, float y, float z) noexcept {
  g_gravity = engine::math::Vec3(x, y, z);
}

engine::math::Vec3 get_gravity() noexcept { return g_gravity; }

void set_collision_dispatch(CollisionDispatchFn fn) noexcept {
  g_collisionDispatch = fn;
}

void dispatch_collision_callbacks() noexcept {
  if ((g_collisionDispatch != nullptr) && (g_collisionPairCount > 0U)) {
    g_collisionDispatch(g_collisionPairData.data(), g_collisionPairCount);
  }
  g_collisionPairCount = 0U;
}

} // namespace engine::physics
