#include "engine/runtime/spring_arm_update.h"

#include <cmath>

#include "engine/core/logging.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

void update_spring_arm_cameras(World &world, float dt) noexcept {
  CameraManager &camMgr = world.camera_manager();

  world.for_each<SpringArmComponent>([&](core::Entity entity,
                                         const SpringArmComponent &arm) {
    math::Transform transform{};
    if (!world.get_transform(entity, &transform)) {
      return;
    }

    // Pivot = entity position + local offset.
    const math::Vec3 pivot(transform.position.x + arm.offset.x,
                           transform.position.y + arm.offset.y,
                           transform.position.z + arm.offset.z);

    // Camera sits behind the entity along its local -Z axis.
    const math::Vec3 localBack(0.0F, 0.0F, 1.0F);
    const math::Vec3 armDir =
        math::normalize(math::rotate_vector(localBack, transform.rotation));

    // Desired arm length — could be shortened by collision in a full impl.
    // For now, smoothly interpolate currentLength toward armLength.
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

    camMgr.push_camera(entity.index, entry, 10.0F);
  });
}

} // namespace engine::runtime
