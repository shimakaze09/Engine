// Declares editor camera types and APIs for the Engine editor tool.

#pragma once

#include "engine/math/mat4.h"
#include "engine/math/ray.h"
#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"

namespace engine::editor {

/// Orbit/pan/zoom camera state for the editor viewport.
struct EditorCamera final {
  math::Vec3 target = math::Vec3(0.0F, 0.0F, 0.0F);
  float yaw = 0.0F;      // radians
  float pitch = 0.3F;    // radians (slight downward look)
  float distance = 8.0F; // distance from target

  static constexpr float kMinPitch = -1.5F;
  static constexpr float kMaxPitch = 1.5F;
  static constexpr float kMinDistance = 0.5F;
  // Far enough to frame a kilometre-wide selection; the clip planes
  // follow the distance (editor_camera_state).
  static constexpr float kMaxDistance = 1000.0F;
};

/// Update the orbit camera from mouse input in the scene viewport.
/// deltaX/deltaY: mouse pixel deltas. scrollDelta: mouse wheel ticks.
/// orbit: Alt+LMB held. pan: Alt+MMB held.
void update_editor_camera(EditorCamera &camera, int deltaX, int deltaY,
                          int scrollDelta, bool orbit, bool pan) noexcept;

/// Convert the editor camera's spherical coordinates to a CameraState.
/// Its clip planes follow the orbit distance d: near max(0.1, 0.0025 d)
/// and far max(100, 2.5 d), so the target stays in view at every zoom and
/// far:near never exceeds 1000.
renderer::CameraState editor_camera_state(const EditorCamera &camera) noexcept;

/// The world-space ray through a viewport point, (ndcX, ndcY) in [-1, 1]
/// with +y up, unprojected through `view` and `projection`: it starts on
/// the near plane and its direction reaches the far plane.
/// `depthZeroToOne` names the projection's clip depth range, [0, 1] on
/// D3D, Metal and Vulkan and [-1, 1] on GL, so the near plane is found
/// under either. False when the matrices are singular.
bool viewport_ray(const math::Mat4 &view, const math::Mat4 &projection,
                  bool depthZeroToOne, float ndcX, float ndcY,
                  math::Ray *out) noexcept;

/// The frustum's eight world-space corners: the near quad, then the far
/// quad, each bottom-left, bottom-right, top-right, top-left. Same
/// conventions as viewport_ray; false when the matrices are singular.
bool frustum_corners(const math::Mat4 &view, const math::Mat4 &projection,
                     bool depthZeroToOne, math::Vec3 (&out)[8]) noexcept;

} // namespace engine::editor
