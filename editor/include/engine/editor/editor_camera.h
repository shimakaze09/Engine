#pragma once

#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"

namespace engine::editor {

struct EditorCamera final {
  math::Vec3 target = math::Vec3(0.0F, 0.0F, 0.0F);
  float yaw = 0.0F;      // radians
  float pitch = 0.3F;    // radians (slight downward look)
  float distance = 8.0F; // distance from target

  static constexpr float kMinPitch = -1.5F;
  static constexpr float kMaxPitch = 1.5F;
  static constexpr float kMinDistance = 0.5F;
  static constexpr float kMaxDistance = 200.0F;
};

/// Update the orbit camera from mouse input in the scene viewport.
/// deltaX/deltaY: mouse pixel deltas. scrollDelta: mouse wheel ticks.
/// orbit: Alt+LMB held. pan: Alt+MMB held.
void update_editor_camera(EditorCamera &camera, int deltaX, int deltaY,
                          int scrollDelta, bool orbit, bool pan) noexcept;

/// Convert the editor camera's spherical coordinates to a CameraState.
renderer::CameraState editor_camera_state(const EditorCamera &camera) noexcept;

} // namespace engine::editor
