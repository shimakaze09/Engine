// Implements spring arm update behavior for the Engine runtime world.

#include "engine/runtime/spring_arm_update.h"

#include <cmath>

#include "engine/core/logging.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics_query.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

/// Advances spring-arm cameras. The pivot composes the hierarchy world
/// transform with the entity-local offset (scaled, then rotated, matching
/// T*R*S child composition; falls back to the local transform when no world
/// transform exists). The camera trails along the world-rotated local +Z.
/// When collision is enabled, a sphere sweep from the pivot (skipping the
/// owning entity and its compound body) clamps the arm to the first hit so
/// the camera never lags through geometry; lag smoothing still governs how
/// the arm extends back toward the authored length. A CameraComponent on
/// the same entity supplies the lens (fov/near/far) and priority/blendSpeed
/// -- the standard authored third-person rig (issue #161) -- and an
/// explicitly disabled one (active == false) suppresses the push entirely;
/// with no CameraComponent authored the previous hardcoded lens/priority
/// stay exactly as before, so existing spring-arm-only scenes are unchanged.
void update_spring_arm_cameras(World &world, float dt) noexcept {
  CameraManager &camMgr = world.camera_manager();

  world.for_each<SpringArmComponent>([&](core::Entity entity,
                                         const SpringArmComponent &arm) {
    const CameraComponent *camComp = world.get_camera_component_ptr(entity);
    if ((camComp != nullptr) && !camComp->active) {
      static_cast<void>(camMgr.pop_camera(entity));
      return;
    }
    math::Vec3 worldPos{};
    math::Quat worldRot{};
    math::Vec3 worldScale(1.0F, 1.0F, 1.0F);
    const WorldTransform *worldTransform =
        world.get_world_transform_read_ptr(entity);
    if (worldTransform != nullptr) {
      worldPos = worldTransform->position;
      worldRot = worldTransform->rotation;
      worldScale = worldTransform->scale;
    } else {
      math::Transform transform{};
      if (!world.get_transform(entity, &transform)) {
        return;
      }
      worldPos = transform.position;
      worldRot = transform.rotation;
      worldScale = transform.scale;
    }

    const math::Vec3 scaledOffset(arm.offset.x * worldScale.x,
                                  arm.offset.y * worldScale.y,
                                  arm.offset.z * worldScale.z);
    const math::Vec3 worldOffset =
        math::rotate_vector(scaledOffset, worldRot);
    const math::Vec3 pivot(worldPos.x + worldOffset.x,
                           worldPos.y + worldOffset.y,
                           worldPos.z + worldOffset.z);

    const math::Vec3 localBack(0.0F, 0.0F, 1.0F);
    const math::Vec3 armDir =
        math::normalize(math::rotate_vector(localBack, worldRot));

    auto *armPtr = world.get_spring_arm_ptr(entity);
    if (armPtr == nullptr) {
      return;
    }

    float desiredLen = arm.armLength;
    bool clipped = false;
    if (arm.collisionEnabled && (desiredLen > 0.0F)) {
      physics::SweepHit hit{};
      if (sweep_sphere(world, pivot, arm.collisionRadius, armDir, desiredLen,
                       &hit, 0xFFFFFFFFU, entity) &&
          (hit.distance < desiredLen)) {
        desiredLen = hit.distance;
        clipped = true;
      }
    }

    const float speed = arm.lagSpeed * dt;
    const float blend = (speed < 1.0F) ? speed : 1.0F;
    float nextLength =
        armPtr->currentLength + (desiredLen - armPtr->currentLength) * blend;
    if (clipped && (desiredLen < nextLength)) {
      nextLength = desiredLen;
    }
    armPtr->currentLength = nextLength;

    const float len = armPtr->currentLength;
    const math::Vec3 camPos(pivot.x + armDir.x * len, pivot.y + armDir.y * len,
                            pivot.z + armDir.z * len);

    CameraEntry entry{};
    entry.position = camPos;
    entry.target = pivot;
    entry.up = math::Vec3(0.0F, 1.0F, 0.0F);
    if (camComp != nullptr) {
      entry.fovRadians = camComp->fovRadians;
      entry.nearPlane = camComp->nearPlane;
      entry.farPlane = camComp->farPlane;
      entry.blendSpeed = camComp->blendSpeed;
    } else {
      entry.blendSpeed = 5.0F;
    }
    const float priority = (camComp != nullptr) ? camComp->priority : 10.0F;

    camMgr.push_camera(entity, entry, priority);
  });
}

} // namespace engine::runtime
