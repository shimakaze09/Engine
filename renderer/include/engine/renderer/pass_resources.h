// Declares pass resources types and APIs for the Engine renderer system.

#pragma once

#include <cstdint>

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
/// Recreates size-dependent targets for the new drawable size.
void resize_pass_resources(int width, int height) noexcept;

/// Current pass-resource set.
const PassResources &get_pass_resources() noexcept;
/// GL texture id backing the resource (0 when absent).
std::uint32_t pass_resource_gpu_texture(PassResourceId resource) noexcept;
/// GL framebuffer id whose color attachment is the resource.
std::uint32_t
pass_resource_framebuffer(PassResourceId colorAttachment) noexcept;

} // namespace engine::renderer
