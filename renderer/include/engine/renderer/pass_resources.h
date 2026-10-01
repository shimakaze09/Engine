// Declares pass resources types and APIs for the Engine renderer system.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/renderer/render_device.h"
#include "engine/renderer/render_view.h"

namespace engine::renderer {

/// Identifies one render-target resource owned by PassResources. The id
/// names the view the target belongs to, so it resolves without any
/// "current view" state.
struct PassResourceId final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const PassResourceId &,
                                   const PassResourceId &) = default;
};

inline constexpr PassResourceId kInvalidPassResource{};

/// One render view's frame targets (scene color/depth, G-buffer, post
/// chains); every view has its own set, sized to its drawable.
struct PassResources final {
  // Scene pass writes:
  PassResourceId sceneColor; // RGBA16F
  PassResourceId sceneDepth; // DEPTH24

  // Post-process reads sceneColor, writes to back buffer (implicit).
  PassResourceId finalColor;

  // G-Buffer pass writes (deferred path):
  PassResourceId gbufferAlbedo;   // RGBA8  — rgb=albedo, a=metallic
  PassResourceId gbufferNormal;   // RGBA16F — rgb=worldNormal, a=roughness
  PassResourceId gbufferEmissive; // RGBA16F — rgb=emissive (HDR), a=AO
  PassResourceId gbufferDepth;    // DEPTH24 — shared with deferred lighting

  // SSAO pass (deferred path):
  PassResourceId ssaoTexture;     // R32F — raw ambient occlusion
  PassResourceId ssaoBlurTexture; // R32F — blurred ambient occlusion
};

/// Creates a view's targets at the given size; a no-op for a view that has
/// them. `view` is a render_view_index; the Game view by default.
bool initialize_pass_resources(int width, int height,
                               std::size_t view = 0U) noexcept;
/// Releases every view's targets.
void shutdown_pass_resources() noexcept;
/// Recreates a view's size-dependent targets for the new drawable size;
/// false when recreation failed and the previous valid targets were kept
/// so the caller can retry at the next size change.
bool resize_pass_resources(int width, int height,
                           std::size_t view = 0U) noexcept;

/// A view's pass-resource set (all ids invalid before it is created).
const PassResources &get_pass_resources(std::size_t view = 0U) noexcept;
/// Device texture backing the resource (invalid when absent).
DeviceTextureHandle pass_resource_texture(PassResourceId resource) noexcept;
/// Render target whose color attachment is the resource (the scene target
/// also carries the scene depth attachment).
RenderTargetHandle
pass_resource_target(PassResourceId colorAttachment) noexcept;

} // namespace engine::renderer
