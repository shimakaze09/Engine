// Implements rigid-body integration for one fixed step: gravity, CCD
// sweeps with snapshot-gated candidates, positional advance, and angular
// velocity integration with light air damping.

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/ccd.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"

namespace engine::physics {

// Light air damping only: contact friction is what actually stops rotation
// on supported bodies, and a heavier value here makes falling boxes right
// themselves in visible slow motion.
constexpr float kAngularDampingPerSecond = 0.4F;

bool step_physics_range(PhysicsWorldView &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept;

bool step_physics(PhysicsWorldView &world, float deltaSeconds) noexcept {
  return step_physics_range(world, 0U, world.transform_count(), deltaSeconds);
}

/// Integrates one dense-index chunk: gravity, then a CCD sweep over every
/// collider owned by each fast rigid-body root (compound children
/// included) keeping the earliest impact — ownership comes from the
/// previous resolve's snapshot, with a direct hierarchy walk only on the
/// first step before a snapshot exists — then positional advance and
/// angular integration.
bool step_physics_range(PhysicsWorldView &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept {
  const PhysicsContext &physicsCtx = world.physics_context();
  const Entity *entities = nullptr;
  const Transform *readTransforms = nullptr;
  Transform *writeTransforms = nullptr;

  if (!world.get_transform_update_range(startIndex, count, &entities,
                                        &readTransforms, &writeTransforms)) {
    return false;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const Entity entity = entities[i];
    if (world.movement_authority(entity) == MovementAuthority::Script) {
      writeTransforms[i] = readTransforms[i];
      continue;
    }

    RigidBody *body = world.get_rigid_body_ptr(entity);
    Transform updated = readTransforms[i];

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

      bool clamped = false;
      CcdSweepResult earliestCcd{};
      const Collider *ownCollider = world.get_collider_ptr(entity);
      if (!physicsCtx.ccdHasCompoundColliders && (ownCollider != nullptr)) {
        earliestCcd =
            bilateral_advance_ccd(world, entity, *body, *ownCollider,
                                  readTransforms[i], deltaSeconds);
      } else if ((physicsCtx.shapeStore != nullptr) &&
                 (physicsCtx.ccdColliderCount > 0U)) {
        const PhysicsShapeStore &store = *physicsCtx.shapeStore;
        for (std::size_t colliderIndex = 0U;
             colliderIndex < physicsCtx.ccdColliderCount; ++colliderIndex) {
          if (store.ccdColliderOwners[colliderIndex] != entity) {
            continue;
          }
          const Entity colliderEntity =
              store.ccdColliderEntities[colliderIndex];
          const Collider *ownedCollider = world.get_collider_ptr(colliderEntity);
          if (ownedCollider == nullptr) {
            continue;
          }
          const CcdSweepResult candidate =
              bilateral_advance_ccd(world, colliderEntity, *body,
                                    *ownedCollider, readTransforms[i],
                                    deltaSeconds);
          if (candidate.hit &&
              (!earliestCcd.hit ||
               (candidate.timeOfImpact < earliestCcd.timeOfImpact))) {
            earliestCcd = candidate;
          }
        }
      } else {
        const std::size_t colliderCount = world.collider_count();
        const Entity *colliderEntities = nullptr;
        const Collider *colliders = nullptr;
        if ((colliderCount > 0U) &&
            world.get_collider_range(0U, colliderCount, &colliderEntities,
                                     &colliders)) {
          for (std::size_t colliderIndex = 0U; colliderIndex < colliderCount;
               ++colliderIndex) {
            if (world.rigid_body_owner(colliderEntities[colliderIndex]) !=
                entity) {
              continue;
            }
            const CcdSweepResult candidate = bilateral_advance_ccd(
                world, colliderEntities[colliderIndex], *body,
                colliders[colliderIndex], readTransforms[i], deltaSeconds);
            if (candidate.hit &&
                (!earliestCcd.hit ||
                 (candidate.timeOfImpact < earliestCcd.timeOfImpact))) {
              earliestCcd = candidate;
            }
          }
        }
      }
      if (earliestCcd.hit) {
        const float safeToi = std::max(0.0F, earliestCcd.timeOfImpact - 0.01F);
        updated.position =
            engine::math::add(readTransforms[i].position,
                              engine::math::mul(displacement, safeToi));

        const float normalVelocity =
            engine::math::dot(body->velocity, earliestCcd.contactNormal);
        if (normalVelocity < 0.0F) {
          body->velocity = engine::math::sub(
              body->velocity, engine::math::mul(earliestCcd.contactNormal,
                                                2.0F * normalVelocity));
        }
        clamped = true;
      }

      if (!clamped) {
        updated.position = engine::math::add(updated.position, displacement);
      }

      const float angularDamping =
          std::exp(-kAngularDampingPerSecond * deltaSeconds);
      body->angularVelocity =
          engine::math::mul(body->angularVelocity, angularDamping);
      const float angSpeedSq = engine::math::length_sq(body->angularVelocity);
      if (angSpeedSq < 1e-6F) {
        body->angularVelocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
      }
    }

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

} // namespace engine::physics
