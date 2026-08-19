// Implements private renderer command buffer context state.

#include "command_buffer_context.h"

#include "engine/math/transform.h"

namespace engine::renderer {

RendererContext &renderer_context() noexcept {
  static RendererContext context{};
  return context;
}

BackendState &backend_state() noexcept {
  return renderer_context().backend;
}

void reset_renderer_public_state() noexcept {
  renderer_context().activeCamera = CameraState{};
  renderer_context().sceneViewportWidth = 0;
  renderer_context().sceneViewportHeight = 0;
  renderer_context().lastFrameStats = RendererFrameStats{};
  renderer_context().fxaaAppliedThisFrame = false;
  renderer_context().activeSkyboxTexture = kInvalidTextureHandle;
  renderer_context().sceneCaptureRequests = {};
  renderer_context().sceneCaptureRequestCount = 0U;
}

void reset_backend_on_failure() noexcept {
  BackendState &backend = backend_state();
  backend = BackendState{};
  backend.failed = true;
}


/// The one projection builder shared by every active-camera consumer
/// (#221): perspective from fovRadians, orthographic from the half-height
/// orthographicSize, both with the historical fov/near/far fallbacks.
math::Mat4 camera_projection_matrix(const CameraState &camera,
                                    float aspect) noexcept {
  const float safeAspect = (aspect > 0.0F) ? aspect : 1.0F;
  const float nearP = (camera.nearPlane > 0.0F) ? camera.nearPlane : 0.1F;
  const float farP = (camera.farPlane > nearP) ? camera.farPlane : 100.0F;
  if (camera.projection == CameraState::kProjectionOrthographic) {
    const float halfH =
        (camera.orthographicSize > 0.0F) ? camera.orthographicSize : 5.0F;
    const float halfW = halfH * safeAspect;
    return math::ortho(-halfW, halfW, -halfH, halfH, nearP, farP);
  }
  const float fov =
      (camera.fovRadians > 0.0F) ? camera.fovRadians : 1.0471975512F;
  return math::perspective(fov, safeAspect, nearP, farP);
}

/// Sky pass lens (#221): perspective directional sampling regardless of the
/// camera's projection kind (see command_buffer_flush_internal.h).
math::Mat4 sky_projection_matrix(const CameraState &camera,
                                 float aspect) noexcept {
  CameraState directional = camera;
  directional.projection = CameraState::kProjectionPerspective;
  return camera_projection_matrix(directional, aspect);
}

} // namespace engine::renderer
