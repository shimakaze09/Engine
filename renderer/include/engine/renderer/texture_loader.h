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

/// How a texture's texels are read. A material slot decides it: base
/// colour and emissive maps are authored in sRGB (as glTF defines them),
/// every other map holds linear data.
enum class TextureColorSpace : std::uint8_t { Linear, Srgb };

/// Loads an image file as a texture. An sRGB colour texture is decoded to
/// RGBA8 and created sRGB, so the device samples it as linear light and
/// its mips are averaged there; an HDR image is linear whatever is asked.
TextureHandle
load_texture(const char *virtualPath,
             TextureColorSpace space = TextureColorSpace::Linear) noexcept;
/// Loads the requested resource for hdr equirect cubemap.
TextureHandle load_hdr_equirect_cubemap(const char *virtualPath,
                                        std::int32_t faceSize) noexcept;
/// Validates texture input size before calling stb's int-length APIs.
bool texture_input_size_fits_stb(std::size_t fileSize,
                                 int *outStbSize) noexcept;
/// Preflights an encoded image header against the decoded-size budget
/// without decoding: false for an unreadable header,
/// dimensions over the 16384 cap, or a decoded byte total over 512 MiB
/// (HDR decodes to 32-bit floats; forcedChannels overrides the header's
/// channel count when the decode will force one, 0 keeps the header's).
/// Logs the rejection with the label and offending numbers.
/// outDecodedBytes (nullable) receives the projected decoded size.
bool texture_decode_within_budget(const unsigned char *encodedBytes,
                                  int encodedByteCount, bool decodeAsHdr,
                                  int forcedChannels, const char *label,
                                  std::uint64_t *outDecodedBytes) noexcept;
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
