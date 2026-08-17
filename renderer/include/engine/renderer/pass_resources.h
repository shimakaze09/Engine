// Declares pass resources types and APIs for the Engine renderer system.

#pragma once

#include <cstdint>

#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Identifies one render-target resource owned by PassResources.
struct PassResourceId final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const PassResourceId &,
                                   const PassResourceId &) = default;
};

inline constexpr PassResourceId kInvalidPassResource{};

/// Frame render targets (scene color/depth, G-buffer, post chains).
struct PassResources final {
  // Scene pass writes:
  PassResourceId sceneColor; // RGBA16F
  PassResourceId sceneDepth; // DEPTH24

  // Post-process reads sceneColor, writes to back buffer (implicit).
  PassResourceId finalColor;

  // G-Buffer pass writes (deferred path):
  PassResourceId gbufferAlbedo;   // RGBA8  — rgb=albedo, a=metallic
  PassResourceId gbufferNormal;   // RGBA16F — rgb=worldNormal, a=roughness
  PassResourceId gbufferEmissive; // RGBA8  — rgb=emissive, a=AO
  PassResourceId gbufferDepth;    // DEPTH24 — shared with deferred lighting

  // SSAO pass (deferred path):
  PassResourceId ssaoTexture;     // R32F — raw ambient occlusion
  PassResourceId ssaoBlurTexture; // R32F — blurred ambient occlusion
};

/// Initializes the owning system for pass resources.
bool initialize_pass_resources(int width, int height) noexcept;
/// Shuts down the owning system for pass resources.
void shutdown_pass_resources() noexcept;
/// Recreates size-dependent targets for the new drawable size; false when
/// recreation failed and the previous valid targets were kept so the
/// caller can retry at the next size change (audit H-12).
bool resize_pass_resources(int width, int height) noexcept;

/// Current pass-resource set.
const PassResources &get_pass_resources() noexcept;
/// Device texture backing the resource (invalid when absent).
DeviceTextureHandle pass_resource_texture(PassResourceId resource) noexcept;
/// Render target whose color attachment is the resource (the scene target
/// also carries the scene depth attachment).
RenderTargetHandle
pass_resource_target(PassResourceId colorAttachment) noexcept;

} // namespace engine::renderer
