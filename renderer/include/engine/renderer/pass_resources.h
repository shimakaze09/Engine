#pragma once

#include <cstdint>

namespace engine::renderer {

struct PassResourceId final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const PassResourceId &,
                                   const PassResourceId &) = default;
};

inline constexpr PassResourceId kInvalidPassResource{};

struct PassResources final {
  // Scene pass writes:
  PassResourceId sceneColor; // RGBA16F
  PassResourceId sceneDepth; // DEPTH24

  // Post-process reads sceneColor, writes to back buffer (implicit).
  PassResourceId finalColor;
};

bool initialize_pass_resources(int width, int height) noexcept;
void shutdown_pass_resources() noexcept;
void resize_pass_resources(int width, int height) noexcept;

const PassResources &get_pass_resources() noexcept;
std::uint32_t pass_resource_gpu_texture(PassResourceId resource) noexcept;
std::uint32_t
pass_resource_framebuffer(PassResourceId colorAttachment) noexcept;

} // namespace engine::renderer
