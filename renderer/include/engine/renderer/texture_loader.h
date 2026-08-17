// Declares texture loader types and APIs for the Engine renderer system.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Opaque id of a loaded texture (0 = invalid).
struct TextureHandle final {
  std::uint32_t id = 0U;

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
/// Releases the texture's device object; the handle becomes stale.
/// External registrations only release the slot — their device texture is
/// owned elsewhere.
void unload_texture(TextureHandle handle) noexcept;
/// Registers a device texture owned elsewhere (e.g. a scene capture
/// target) behind a stable handle the material path can reference. The
/// texture system never destroys the device object; an invalid handle is
/// allowed and simply resolves to "no texture" until updated.
TextureHandle register_external_texture(DeviceTextureHandle texture) noexcept;
/// Repoints an externally registered handle at a new device texture;
/// false when the handle is stale or not external.
bool update_external_texture(TextureHandle handle,
                             DeviceTextureHandle texture) noexcept;
/// Device texture behind the handle (invalid when stale).
DeviceTextureHandle texture_device_handle(TextureHandle handle) noexcept;
/// Returns whether is texture hdr.
bool is_texture_hdr(TextureHandle handle) noexcept;
/// Returns whether is texture cubemap.
bool is_texture_cubemap(TextureHandle handle) noexcept;

} // namespace engine::renderer
