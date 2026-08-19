// Implements scene-capture (render-to-texture) request storage and the
// device render-target slots behind them. The capture pass itself runs inside
// flush_renderer (command_buffer_flush.cpp) so it can share the forward-path
// state; this TU owns everything that is not per-frame pass code.

#include "command_buffer_capture.h"

#include <algorithm>

#include "engine/core/logging.h"
#include "engine/renderer/command_buffer.h"

namespace engine::renderer {

namespace {

constexpr const char *kCaptureLogChannel = "renderer";

/// Releases one capture target's device objects (safe on empty slots). The
/// external texture handle survives so material references stay stable
/// across target resizes; it resolves to "no texture" until re-created.
void destroy_capture_target(SceneCaptureTarget &target,
                            const RenderDevice *dev) noexcept {
  if (dev != nullptr) {
    if ((target.target.value != 0U) &&
        (dev->destroy_render_target != nullptr)) {
      dev->destroy_render_target(target.target);
    }
    if (dev->destroy_texture != nullptr) {
      if (target.depthTexture != kInvalidDeviceTexture) {
        dev->destroy_texture(target.depthTexture);
      }
      if (target.colorTexture != kInvalidDeviceTexture) {
        dev->destroy_texture(target.colorTexture);
      }
    }
  }
  const TextureHandle preservedHandle = target.textureHandle;
  target = SceneCaptureTarget{};
  target.textureHandle = preservedHandle;
  static_cast<void>(
      update_external_texture(preservedHandle, kInvalidDeviceTexture));
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
  if (normalized.camera.orthographicSize <= 0.0F) {
    normalized.camera.orthographicSize = CameraState{}.orthographicSize;
  }
  return normalized;
}

/// Stores this frame's normalized capture requests and gives each slot a
/// stable external texture-system handle so render prep can reference the
/// capture output as a material texture. Registration happens on the render
/// thread only; render prep merely reads the handle.
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

DeviceTextureHandle get_scene_capture_texture(std::size_t index) noexcept {
  if (index >= kMaxSceneCaptures) {
    return kInvalidDeviceTexture;
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
  if ((target.target.value != 0U) && (target.width == width) &&
      (target.height == height)) {
    return true;
  }

  destroy_capture_target(target, dev);
  if ((dev->create_texture == nullptr) ||
      (dev->create_render_target == nullptr)) {
    return false;
  }

  // Single-level LDR color: the capture pass renders mip 0 only, so a
  // generated chain would freeze stale data (issue #229); material
  // sampling clamps to the rendered level instead.
  TextureDesc colorDesc{};
  colorDesc.kind = TextureKind::Tex2D;
  colorDesc.format = TextureFormat::RGBA8;
  colorDesc.width = width;
  colorDesc.height = height;
  colorDesc.filter = TextureFilter::Linear;
  colorDesc.wrap = TextureWrap::Repeat;
  target.colorTexture = dev->create_texture(colorDesc);
  if (target.colorTexture == kInvalidDeviceTexture) {
    core::log_message(core::LogLevel::Error, kCaptureLogChannel,
                      "failed to create scene capture color texture");
    destroy_capture_target(target, dev);
    return false;
  }

  TextureDesc depthDesc{};
  depthDesc.kind = TextureKind::Tex2D;
  depthDesc.format = TextureFormat::Depth24;
  depthDesc.width = width;
  depthDesc.height = height;
  depthDesc.filter = TextureFilter::Linear;
  depthDesc.wrap = TextureWrap::Repeat;
  target.depthTexture = dev->create_texture(depthDesc);
  if (target.depthTexture == kInvalidDeviceTexture) {
    core::log_message(core::LogLevel::Error, kCaptureLogChannel,
                      "failed to create scene capture depth texture");
    destroy_capture_target(target, dev);
    return false;
  }

  RenderTargetDesc targetDesc{};
  targetDesc.colorCount = 1U;
  targetDesc.colors[0].texture = target.colorTexture;
  targetDesc.depth.texture = target.depthTexture;
  target.target = dev->create_render_target(targetDesc);
  if (target.target.value == 0U) {
    core::log_message(core::LogLevel::Error, kCaptureLogChannel,
                      "failed to create scene capture render target");
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
