// Implements authored CameraComponent publishing into CameraManager and the
// authoring-time active-camera selection query.

#include "engine/runtime/camera_component_update.h"

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

/// Derives each authored camera's pose from its world transform (looking
/// along the rotated -Z axis, up is the rotated +Y axis, matching
/// SceneCaptureComponent's convention -- see engine_frame_collect.cpp's
/// collect_scene_captures) and republishes it into CameraManager every
/// frame so authored cameras blend/prioritize/shake exactly like Lua-pushed
/// ones. dt is accepted for signature symmetry with update_spring_arm_cameras
/// (CameraManager's own evaluate() owns all time-based blending); the pose
/// push itself is instantaneous.
void update_persistent_cameras(World &world, float dt) noexcept {
  static_cast<void>(dt);
  CameraManager &camMgr = world.camera_manager();

  world.for_each<CameraComponent>([&](core::Entity entity,
                                      const CameraComponent &camera) {
    // A same-entity SpringArmComponent already published this owner's pose
    // (and read this CameraComponent for lens/priority/blend) earlier this
    // frame; pushing again here with the Transform-derived pose instead of
    // the spring arm's collision-aware boom would silently fight it.
    if (world.has_spring_arm(entity)) {
      return;
    }

    if (!camera.active) {
      static_cast<void>(camMgr.pop_camera(entity));
      return;
    }

    math::Vec3 position(0.0F, 0.0F, 0.0F);
    math::Quat rotation{};
    const WorldTransform *worldTransform =
        world.get_world_transform_read_ptr(entity);
    if (worldTransform != nullptr) {
      position = worldTransform->position;
      rotation = worldTransform->rotation;
    } else {
      Transform local{};
      if (!world.get_transform(entity, &local)) {
        return;
      }
      position = local.position;
      rotation = local.rotation;
    }

    CameraEntry entry{};
    entry.position = position;
    entry.target = math::add(
        position, math::rotate_vector(math::Vec3(0.0F, 0.0F, -1.0F), rotation));
    entry.up = math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), rotation);
    entry.fovRadians = camera.fovRadians;
    entry.nearPlane = camera.nearPlane;
    entry.farPlane = camera.farPlane;
    entry.blendSpeed = camera.blendSpeed;

    static_cast<void>(camMgr.push_camera(entity, entry, camera.priority));
  });
}

core::Entity find_authored_active_camera(const World &world,
                                         std::uint32_t *outTieCount) noexcept {
  core::Entity best = kInvalidEntity;
  float bestPriority = 0.0F;
  std::uint32_t tieCount = 0U;

  world.for_each<CameraComponent>([&](core::Entity entity,
                                      const CameraComponent &camera) {
    if (!camera.active) {
      return;
    }
    if (best == kInvalidEntity) {
      best = entity;
      bestPriority = camera.priority;
      tieCount = 0U;
    } else if (camera.priority > bestPriority) {
      best = entity;
      bestPriority = camera.priority;
      tieCount = 0U;
    } else if (camera.priority == bestPriority) {
      ++tieCount;
    }
  });

  if (outTieCount != nullptr) {
    *outTieCount = tieCount;
  }
  return best;
}

} // namespace engine::runtime
