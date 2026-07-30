// Implements spring arm update behavior for the Engine runtime world.

#include "engine/runtime/spring_arm_update.h"

#include <cmath>

#include "engine/core/logging.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

/// Advances spring-arm cameras: the camera trails the entity along its
/// local +Z with the arm length smoothed toward the target (collision
/// shortening is not implemented yet).
void update_spring_arm_cameras(World &world, float dt) noexcept {
  CameraManager &camMgr = world.camera_manager();

  world.for_each<SpringArmComponent>([&](core::Entity entity,
                                         const SpringArmComponent &arm) {
    math::Transform transform{};
    if (!world.get_transform(entity, &transform)) {
      return;
    }

      const math::Vec3 pivot(transform.position.x + arm.offset.x,
                           transform.position.y + arm.offset.y,
                           transform.position.z + arm.offset.z);

      const math::Vec3 localBack(0.0F, 0.0F, 1.0F);
    const math::Vec3 armDir =
        math::normalize(math::rotate_vector(localBack, transform.rotation));

    auto *armPtr = world.get_spring_arm_ptr(entity);
    if (armPtr == nullptr) {
      return;
    }

    const float targetLen = arm.armLength;
    const float speed = arm.lagSpeed * dt;
    const float blend = (speed < 1.0F) ? speed : 1.0F;
    armPtr->currentLength =
        armPtr->currentLength + (targetLen - armPtr->currentLength) * blend;

    const float len = armPtr->currentLength;
    const math::Vec3 camPos(pivot.x + armDir.x * len, pivot.y + armDir.y * len,
                            pivot.z + armDir.z * len);

    CameraEntry entry{};
    entry.position = camPos;
    entry.target = pivot;
    entry.up = math::Vec3(0.0F, 1.0F, 0.0F);
    entry.blendSpeed = 5.0F;

    camMgr.push_camera(entity, entry, 10.0F);
  });
}

} // namespace engine::runtime
