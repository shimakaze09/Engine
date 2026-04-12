#pragma once

#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"

namespace engine::editor {

/// Free-fly debug camera (WASD + mouse) for detaching from the game camera.
struct DebugCamera final {
  math::Vec3 position = math::Vec3(0.0F, 5.0F, 10.0F);
  float yaw = 0.0F;   // radians
  float pitch = 0.0F; // radians
  float moveSpeed = 8.0F;
  float lookSensitivity = 0.003F;

  static constexpr float kMinPitch = -1.5F;
  static constexpr float kMaxPitch = 1.5F;
};

/// Update the debug camera from keyboard + mouse input.
/// wasd: forward/left/back/right booleans. shift: speed boost.
/// mouseDeltaX/Y: raw mouse deltas while right-click is held.
void update_debug_camera(DebugCamera &camera, float dt, bool forward, bool back,
                         bool left, bool right, bool up, bool down, bool shift,
                         int mouseDeltaX, int mouseDeltaY) noexcept;

/// Convert the debug camera state to a CameraState for rendering.
renderer::CameraState debug_camera_state(const DebugCamera &camera) noexcept;

/// Draw the frozen game camera frustum as debug wireframe lines.
/// Uses the debug_draw API from core to render 12 edges of the frustum.
void draw_camera_frustum_wireframe(const renderer::CameraState &frozenCamera,
                                   float aspectRatio) noexcept;

} // namespace engine::editor
