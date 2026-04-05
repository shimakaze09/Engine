#include "engine/physics/physics.h"

#include <algorithm>

#include "engine/math/vec3.h"

namespace engine::physics {

namespace {

constexpr float kStaticInverseMass = 0.0F;
constexpr engine::math::Vec3 kGravity(0.0F, -9.8F, 0.0F);

float axis_overlap(float aMin, float aMax, float bMin, float bMax) noexcept {
  const float left = std::max(aMin, bMin);
  const float right = std::min(aMax, bMax);
  return right - left;
}

float sign_or_positive(float value) noexcept {
  return (value < 0.0F) ? -1.0F : 1.0F;
}

} // namespace

bool step_physics(runtime::World &world, float deltaSeconds) noexcept {
  return step_physics_range(world, 0U, world.transform_count(), deltaSeconds);
}

bool step_physics_range(runtime::World &world,
                        std::size_t startIndex,
                        std::size_t count,
                        float deltaSeconds) noexcept {
  const runtime::Entity *entities = nullptr;
  const runtime::Transform *readTransforms = nullptr;
  runtime::Transform *writeTransforms = nullptr;

  if (!world.get_transform_update_range(
          startIndex, count, &entities, &readTransforms, &writeTransforms)) {
    return false;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = entities[i];
    if (world.movement_authority(entity)
        == runtime::MovementAuthority::Script) {
      writeTransforms[i] = readTransforms[i];
      continue;
    }

    runtime::RigidBody *body = world.get_rigid_body_ptr(entity);
    runtime::Transform updated = readTransforms[i];

    if ((body != nullptr) && (body->inverseMass > 0.0F)) {
      const engine::math::Vec3 totalAccel =
          engine::math::add(body->acceleration, kGravity);
      body->velocity = engine::math::add(
          body->velocity, engine::math::mul(totalAccel, deltaSeconds));
      updated.position = engine::math::add(
          updated.position, engine::math::mul(body->velocity, deltaSeconds));
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

  // Collision resolution runs after physics integration and reads/writes the
  // update buffer only, so it sees the integrated positions across all chunks.
  for (std::size_t i = 0U; i < colliderCount; ++i) {
    const runtime::Entity entityA = entities[i];
    if (world.movement_authority(entityA)
        == runtime::MovementAuthority::Script) {
      continue;
    }

    const runtime::Transform *transformA =
        world.get_transform_write_ptr(entityA);
    if (transformA == nullptr) {
      continue;
    }

    for (std::size_t j = i + 1U; j < colliderCount; ++j) {
      const runtime::Entity entityB = entities[j];
      if (world.movement_authority(entityB)
          == runtime::MovementAuthority::Script) {
        continue;
      }

      const runtime::Transform *transformB =
          world.get_transform_write_ptr(entityB);
      if (transformB == nullptr) {
        continue;
      }

      const runtime::Collider &colliderA = colliders[i];
      const runtime::Collider &colliderB = colliders[j];

      const float ax = transformA->position.x;
      const float ay = transformA->position.y;
      const float az = transformA->position.z;
      const float bx = transformB->position.x;
      const float by = transformB->position.y;
      const float bz = transformB->position.z;

      const float overlapX = axis_overlap(ax - colliderA.halfExtents.x,
                                          ax + colliderA.halfExtents.x,
                                          bx - colliderB.halfExtents.x,
                                          bx + colliderB.halfExtents.x);
      if (overlapX <= 0.0F) {
        continue;
      }

      const float overlapY = axis_overlap(ay - colliderA.halfExtents.y,
                                          ay + colliderA.halfExtents.y,
                                          by - colliderB.halfExtents.y,
                                          by + colliderB.halfExtents.y);
      if (overlapY <= 0.0F) {
        continue;
      }

      const float overlapZ = axis_overlap(az - colliderA.halfExtents.z,
                                          az + colliderA.halfExtents.z,
                                          bz - colliderB.halfExtents.z,
                                          bz + colliderB.halfExtents.z);
      if (overlapZ <= 0.0F) {
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

      const runtime::RigidBody *bodyA = world.get_rigid_body_ptr(entityA);
      const runtime::RigidBody *bodyB = world.get_rigid_body_ptr(entityB);
      const float invMassA =
          (bodyA != nullptr) ? bodyA->inverseMass : kStaticInverseMass;
      const float invMassB =
          (bodyB != nullptr) ? bodyB->inverseMass : kStaticInverseMass;
      const float invMassSum = invMassA + invMassB;

      if (invMassSum <= 0.0F) {
        continue;
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
    }
  }

  return true;
}

} // namespace engine::physics
