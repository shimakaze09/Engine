// Declares camera types and APIs for the Engine renderer system.

#pragma once

#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"

namespace engine::renderer {

/// Position/target/up plus projection parameters for a camera.
struct CameraState final {
  /// Projection kinds mirrored from the runtime's CameraProjection encoding
  /// (a plain uint32 so the renderer never includes runtime headers).
  static constexpr std::uint32_t kProjectionPerspective = 0U;
  static constexpr std::uint32_t kProjectionOrthographic = 1U;

  math::Vec3 position = math::Vec3(0.0F, 2.0F, 5.0F);
  math::Vec3 target   = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 up       = math::Vec3(0.0F, 1.0F, 0.0F);
  float fovRadians    = 1.0471975512F; // 60 degrees; used when Perspective
  float nearPlane     = 0.1F;
  float farPlane      = 100.0F;
  std::uint32_t projection = kProjectionPerspective;
  float orthographicSize = 5.0F; ///< Half-height, world units; Orthographic.
};

// Builds the camera's projection matrix for the given aspect with the
// flush path's sanitization (fov/near/far/ortho-size fallbacks). Every
// consumer of the active camera's projection — the GL flush, render-prep
// CPU culling, scene captures, the editor's frustum gizmo — must build
// through this one helper so they can never disagree (#221).
math::Mat4 camera_projection_matrix(const CameraState &camera,
                                    float aspect) noexcept;

/// Sets the requested value for active camera.
void       set_active_camera(const CameraState &camera) noexcept;
/// The camera state the renderer will use this frame.
CameraState get_active_camera() noexcept;

} // namespace engine::renderer
