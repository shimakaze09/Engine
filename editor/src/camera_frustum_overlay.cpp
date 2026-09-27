// Implements the camera frustum overlay declared in
// camera_frustum_overlay.h.

#include "engine/editor/camera_frustum_overlay.h"

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
  // true frustum shape for orthographic cameras too.
  const math::Mat4 proj =
      renderer::camera_projection_matrix(camera, aspectRatio);
  const math::Mat4 vp = math::mul(proj, view);

  math::Mat4 invVP{};
  if (!math::inverse(vp, &invVP)) {
    return;
  }

  // NDC corners: 8 corners of the unit cube [-1,1]^3
  // near z = -1, far z = 1 in OpenGL NDC
  constexpr float kNDC[8][4] = {
      {-1.0F, -1.0F, -1.0F, 1.0F}, // near-bottom-left
      {1.0F, -1.0F, -1.0F, 1.0F},  // near-bottom-right
      {1.0F, 1.0F, -1.0F, 1.0F},   // near-top-right
      {-1.0F, 1.0F, -1.0F, 1.0F},  // near-top-left
      {-1.0F, -1.0F, 1.0F, 1.0F},  // far-bottom-left
      {1.0F, -1.0F, 1.0F, 1.0F},   // far-bottom-right
      {1.0F, 1.0F, 1.0F, 1.0F},    // far-top-right
      {-1.0F, 1.0F, 1.0F, 1.0F},   // far-top-left
  };

  math::Vec3 corners[8]{};
  for (int i = 0; i < 8; ++i) {
    const math::Vec4 ndc(kNDC[i][0], kNDC[i][1], kNDC[i][2], kNDC[i][3]);
    const math::Vec4 world = math::mul(invVP, ndc);
    if (std::fabs(world.w) < 0.0001F) {
      return;
    }
    const float invW = 1.0F / world.w;
    corners[i] = math::Vec3(world.x * invW, world.y * invW, world.z * invW);
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
