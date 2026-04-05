#pragma once

#include <cstdint>

namespace engine::renderer {

struct TextureHandle final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const TextureHandle &,
                                   const TextureHandle &) = default;
};

inline constexpr TextureHandle kInvalidTextureHandle{};

bool initialize_texture_system() noexcept;
void shutdown_texture_system() noexcept;

TextureHandle load_texture(const char *virtualPath) noexcept;
void unload_texture(TextureHandle handle) noexcept;
std::uint32_t texture_gpu_id(TextureHandle handle) noexcept;
bool is_texture_hdr(TextureHandle handle) noexcept;

} // namespace engine::renderer
