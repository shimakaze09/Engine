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
  renderer_context().gameViewWidth = 0;
  renderer_context().gameViewHeight = 0;
  renderer_context().lastFrameStats = RendererFrameStats{};
  renderer_context().activeSkyboxTexture = kInvalidTextureHandle;
  renderer_context().sceneCaptureRequests = {};
  renderer_context().sceneCaptureRequestCount = 0U;
  renderer_context().reflectionProbeRequests = {};
  renderer_context().reflectionProbeRequestCount = 0U;
  renderer_context().activeReflectionProbe.fill(-1);
  // Stored skin palettes outlive the frame that flushed them and disable
  // the directional shadow cache while any is present; a run that ends
  // with skinned meshes must not leave the next run's cache off.
  renderer_context().skinPaletteCount = 0U;
}

void reset_backend_on_failure() noexcept {
  BackendState &backend = backend_state();
  backend = BackendState{};
  backend.failed = true;
}


/// The one projection builder shared by every active-camera consumer:
/// perspective from fovRadians, orthographic from the half-height
/// orthographicSize, both with the historical fov/near/far fallbacks.
/// Device clip/texture conventions (defaults when no device); here in
/// the context TU so the slim per-TU test harnesses that link
/// projection code resolve them without the full command buffer.
bool device_depth_zero_one() noexcept {
  const RenderDevice *dev = render_device();
  return (dev != nullptr) && dev->caps.depthZeroToOne;
}

bool device_target_origin_bottom_left() noexcept {
  const RenderDevice *dev = render_device();
  return (dev == nullptr) || dev->caps.textureOriginBottomLeft;
}

CameraDepthRange camera_depth_range(const CameraState &camera) noexcept {
  CameraDepthRange range{};
  range.nearPlane = (camera.nearPlane > 0.0F) ? camera.nearPlane : 0.1F;
  range.farPlane =
      (camera.farPlane > range.nearPlane) ? camera.farPlane : 100.0F;
  return range;
}

math::Mat4 camera_projection_matrix(const CameraState &camera,
                                    float aspect) noexcept {
  const float safeAspect = (aspect > 0.0F) ? aspect : 1.0F;
  const CameraDepthRange range = camera_depth_range(camera);
  const float nearP = range.nearPlane;
  const float farP = range.farPlane;
  if (math::projection_is_orthographic(camera.projection)) {
    const float halfH =
        (camera.orthographicSize > 0.0F) ? camera.orthographicSize : 5.0F;
    const float halfW = halfH * safeAspect;
    return device_depth_zero_one()
               ? math::ortho_zero_one(-halfW, halfW, -halfH, halfH, nearP,
                                      farP)
               : math::ortho(-halfW, halfW, -halfH, halfH, nearP, farP);
  }
  const float fov =
      (camera.fovRadians > 0.0F) ? camera.fovRadians : 1.0471975512F;
  return device_depth_zero_one()
             ? math::perspective_zero_one(fov, safeAspect, nearP, farP)
             : math::perspective(fov, safeAspect, nearP, farP);
}

/// Sky pass lens: perspective directional sampling regardless of the
/// camera's projection kind (see command_buffer_flush_internal.h).
math::Mat4 sky_projection_matrix(const CameraState &camera,
                                 float aspect) noexcept {
  CameraState directional = camera;
  directional.projection = CameraState::kProjectionPerspective;
  return camera_projection_matrix(directional, aspect);
}

} // namespace engine::renderer
