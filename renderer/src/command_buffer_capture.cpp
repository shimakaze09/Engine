// Implements scene-capture (render-to-texture) request storage and the GL
// render-target slots behind them. The capture pass itself runs inside
// flush_renderer (command_buffer_flush.cpp) so it can share the forward-path
// state; this TU owns everything that is not per-frame pass code.

#include "command_buffer_capture.h"

#include <algorithm>

#include "engine/core/logging.h"
#include "engine/renderer/command_buffer.h"

namespace engine::renderer {

namespace {

constexpr const char *kCaptureLogChannel = "renderer";

/// Releases one capture target's GL objects (safe on empty slots). The
/// external texture handle survives so material references stay stable
/// across target resizes; it resolves to "no texture" until re-created.
void destroy_capture_target(SceneCaptureTarget &target,
                            const RenderDevice *dev) noexcept {
  if (dev != nullptr) {
    if (target.framebuffer != 0U) {
      dev->destroy_framebuffer(target.framebuffer);
    }
    if (target.depthTexture != 0U) {
      dev->destroy_texture(target.depthTexture);
    }
    if (target.colorTexture != 0U) {
      dev->destroy_texture(target.colorTexture);
    }
  }
  const TextureHandle preservedHandle = target.textureHandle;
  target = SceneCaptureTarget{};
  target.textureHandle = preservedHandle;
  static_cast<void>(update_external_texture(preservedHandle, 0U));
}

} // namespace

SceneCaptureRequest normalize_scene_capture_request(
    const SceneCaptureRequest &request) noexcept {
  SceneCaptureRequest normalized = request;
  normalized.width =
      std::clamp(normalized.width, kMinSceneCaptureSize, kMaxSceneCaptureSize);
  normalized.height =
      std::clamp(normalized.height, kMinSceneCaptureSize, kMaxSceneCaptureSize);
  if (normalized.camera.fovRadians <= 0.0F) {
    normalized.camera.fovRadians = CameraState{}.fovRadians;
  }
  if (normalized.camera.nearPlane <= 0.0F) {
    normalized.camera.nearPlane = CameraState{}.nearPlane;
  }
  if (normalized.camera.farPlane <= normalized.camera.nearPlane) {
    normalized.camera.farPlane = normalized.camera.nearPlane + 100.0F;
  }
  return normalized;
}

void set_scene_capture_requests(const SceneCaptureRequest *requests,
                                std::size_t count) noexcept {
  RendererContext &context = renderer_context();
  if ((requests == nullptr) && (count > 0U)) {
    core::log_message(core::LogLevel::Error, kCaptureLogChannel,
                      "scene capture request array is null");
    context.sceneCaptureRequestCount = 0U;
    return;
  }

  if (count > kMaxSceneCaptures) {
    core::log_message(core::LogLevel::Warning, kCaptureLogChannel,
                      "scene capture requests exceed slot count; extra "
                      "captures dropped");
    count = kMaxSceneCaptures;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    context.sceneCaptureRequests[i] =
        normalize_scene_capture_request(requests[i]);
    // Give each requested slot a stable external texture-system handle so
    // render prep can reference the capture output as a material texture.
    // Registration is render-thread only; render prep merely reads it.
    SceneCaptureTarget &target = context.backend.sceneCaptureTargets[i];
    if (target.textureHandle == kInvalidTextureHandle) {
      target.textureHandle = register_external_texture(target.colorTexture);
    }
  }
  context.sceneCaptureRequestCount = count;
}

std::size_t scene_capture_request_count() noexcept {
  return renderer_context().sceneCaptureRequestCount;
}

std::uint32_t get_scene_capture_texture(std::size_t index) noexcept {
  if (index >= kMaxSceneCaptures) {
    return 0U;
  }
  return backend_state().sceneCaptureTargets[index].colorTexture;
}

TextureHandle scene_capture_texture_handle(std::size_t index) noexcept {
  if (index >= kMaxSceneCaptures) {
    return kInvalidTextureHandle;
  }
  return backend_state().sceneCaptureTargets[index].textureHandle;
}

bool ensure_scene_capture_target(BackendState &backend,
                                 const RenderDevice *dev, std::size_t slot,
                                 int width, int height) noexcept {
  if ((slot >= kMaxSceneCaptures) || (dev == nullptr) || (width <= 0) ||
      (height <= 0)) {
    return false;
  }

  SceneCaptureTarget &target = backend.sceneCaptureTargets[slot];
  if ((target.framebuffer != 0U) && (target.width == width) &&
      (target.height == height)) {
    return true;
  }

  destroy_capture_target(target, dev);

  // Same recipe as the pass-resource finalColor target: LDR color + DEPTH24.
  target.colorTexture = dev->create_texture_2d(width, height, 4, nullptr);
  if (target.colorTexture == 0U) {
    core::log_message(core::LogLevel::Error, kCaptureLogChannel,
                      "failed to create scene capture color texture");
    destroy_capture_target(target, dev);
    return false;
  }

  target.depthTexture = dev->create_depth_texture(width, height);
  if (target.depthTexture == 0U) {
    core::log_message(core::LogLevel::Error, kCaptureLogChannel,
                      "failed to create scene capture depth texture");
    destroy_capture_target(target, dev);
    return false;
  }

  target.framebuffer =
      dev->create_framebuffer(target.colorTexture, target.depthTexture);
  if (target.framebuffer == 0U) {
    core::log_message(core::LogLevel::Error, kCaptureLogChannel,
                      "failed to create scene capture framebuffer");
    destroy_capture_target(target, dev);
    return false;
  }

  target.width = width;
  target.height = height;
  static_cast<void>(
      update_external_texture(target.textureHandle, target.colorTexture));
  return true;
}

void destroy_scene_capture_targets(BackendState &backend,
                                   const RenderDevice *dev) noexcept {
  for (SceneCaptureTarget &target : backend.sceneCaptureTargets) {
    destroy_capture_target(target, dev);
    if (target.textureHandle != kInvalidTextureHandle) {
      unload_texture(target.textureHandle);
      target.textureHandle = kInvalidTextureHandle;
    }
  }
}

} // namespace engine::renderer
