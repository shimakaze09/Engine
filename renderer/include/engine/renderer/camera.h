// Declares camera types and APIs for the Engine renderer system.

#pragma once

#include "engine/math/vec3.h"

namespace engine::renderer {

/// Stores camera state data used by the engine.
struct CameraState final {
  math::Vec3 position = math::Vec3(0.0F, 2.0F, 5.0F);
  math::Vec3 target   = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 up       = math::Vec3(0.0F, 1.0F, 0.0F);
  float fovRadians    = 1.0471975512F; // 60 degrees
  float nearPlane     = 0.1F;
  float farPlane      = 100.0F;
};

/// Sets the requested value for active camera.
void       set_active_camera(const CameraState &camera) noexcept;
/// Returns the requested value for active camera.
CameraState get_active_camera() noexcept;

} // namespace engine::renderer
