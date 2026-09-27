// Implements the camera frustum overlay declared in
// camera_frustum_overlay.h.

#include "engine/editor/camera_frustum_overlay.h"

#include "engine/editor/editor_camera.h"
#include "engine/renderer/command_buffer.h"

#include <cmath>

#include "engine/core/debug_draw.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"

namespace engine::editor {

void draw_camera_frustum_wireframe(const renderer::CameraState &camera,
                                   float aspectRatio) noexcept {
  const math::Mat4 view =
      math::look_at(camera.position, camera.target, camera.up);
  // Shares the renderer's projection builder so the overlay draws the
  // true frustum shape for orthographic cameras too, and unprojects with
  // the device's clip depth range: on a [0, 1] device the GL cube's near
  // face at -1 lands between the eye and the real near plane.
  const math::Mat4 proj =
      renderer::camera_projection_matrix(camera, aspectRatio);
  math::Vec3 corners[8]{};
  if (!frustum_corners(view, proj, renderer::device_depth_zero_one(),
                       corners)) {
    return;
  }

  const core::DebugColor color{1.0F, 1.0F, 0.0F, 1.0F};

  auto toDbg = [](const math::Vec3 &v) -> core::DebugVec3 {
    return {v.x, v.y, v.z};
  };

  core::debug_draw_line(toDbg(corners[0]), toDbg(corners[1]), color);
  core::debug_draw_line(toDbg(corners[1]), toDbg(corners[2]), color);
  core::debug_draw_line(toDbg(corners[2]), toDbg(corners[3]), color);
  core::debug_draw_line(toDbg(corners[3]), toDbg(corners[0]), color);

  core::debug_draw_line(toDbg(corners[4]), toDbg(corners[5]), color);
  core::debug_draw_line(toDbg(corners[5]), toDbg(corners[6]), color);
  core::debug_draw_line(toDbg(corners[6]), toDbg(corners[7]), color);
  core::debug_draw_line(toDbg(corners[7]), toDbg(corners[4]), color);

  core::debug_draw_line(toDbg(corners[0]), toDbg(corners[4]), color);
  core::debug_draw_line(toDbg(corners[1]), toDbg(corners[5]), color);
  core::debug_draw_line(toDbg(corners[2]), toDbg(corners[6]), color);
  core::debug_draw_line(toDbg(corners[3]), toDbg(corners[7]), color);
}

} // namespace engine::editor
