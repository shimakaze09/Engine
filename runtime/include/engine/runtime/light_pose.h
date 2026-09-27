// Declares where a light is and which way it points in the world: the one
// derivation the renderer's light collection and the editor's light
// gizmos share, so a gizmo cannot disagree with the light it draws.

#pragma once

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

/// A light's world position and aim.
struct LightPose final {
  math::Vec3 position = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 direction = math::Vec3(0.0F, 0.0F, 0.0F);
};

/// The pose of a light on `entity` whose component points along
/// `localDirection`: the entity's world position, and the direction turned
/// by its world rotation (not normalized; the renderer normalizes). An
/// entity without a world transform lights from the origin along the
/// direction as authored.
inline LightPose light_world_pose(const World &world, Entity entity,
                                  const math::Vec3 &localDirection) noexcept {
  const WorldTransform *worldTransform =
      world.get_world_transform_read_ptr(entity);
  if (worldTransform == nullptr) {
    return LightPose{math::Vec3(0.0F, 0.0F, 0.0F), localDirection};
  }
  return LightPose{
      worldTransform->position,
      math::rotate_vector(localDirection, worldTransform->rotation)};
}

} // namespace engine::runtime
