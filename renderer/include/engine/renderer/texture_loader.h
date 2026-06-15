// Declares texture loader types and APIs for the Engine renderer system.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

/// Stores texture handle data used by the engine.
struct TextureHandle final {
  std::uint32_t id = 0U;

  /// Compares values for equality.
  friend constexpr bool operator==(const TextureHandle &,
                                   const TextureHandle &) = default;
};

inline constexpr TextureHandle kInvalidTextureHandle{};

/// Initializes the owning system for texture system.
bool initialize_texture_system() noexcept;
/// Shuts down the owning system for texture system.
void shutdown_texture_system() noexcept;

/// Loads the requested resource for texture.
TextureHandle load_texture(const char *virtualPath) noexcept;
/// Loads the requested resource for hdr equirect cubemap.
TextureHandle load_hdr_equirect_cubemap(const char *virtualPath,
                                        std::int32_t faceSize) noexcept;
/// Validates texture input size before calling stb's int-length APIs.
bool texture_input_size_fits_stb(std::size_t fileSize,
                                 int *outStbSize) noexcept;
/// Handles unload texture.
void unload_texture(TextureHandle handle) noexcept;
/// Handles texture gpu id.
std::uint32_t texture_gpu_id(TextureHandle handle) noexcept;
/// Returns whether is texture hdr.
bool is_texture_hdr(TextureHandle handle) noexcept;
/// Returns whether is texture cubemap.
bool is_texture_cubemap(TextureHandle handle) noexcept;

} // namespace engine::renderer
