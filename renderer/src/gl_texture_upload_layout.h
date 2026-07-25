// Selects OpenGL texture formats and scopes pixel-unpack alignment changes.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer::detail {

/// OpenGL format and row-alignment values for one texture upload.
struct GlTextureUploadLayout final {
  std::uint32_t externalFormat = 0U;
  std::int32_t internalFormat = 0;
  std::int32_t unpackAlignment = 1;
};

/// Resolves formats and a valid GL_UNPACK_ALIGNMENT for tightly packed rows.
inline bool describe_gl_texture_upload(
    std::int32_t width, std::int32_t channels,
    std::int32_t bytesPerChannel, bool hdr,
    GlTextureUploadLayout *outLayout) noexcept {
  if (outLayout == nullptr) {
    return false;
  }
  *outLayout = GlTextureUploadLayout{};
  if ((width <= 0) || (channels < 1) || (channels > 4) ||
      (bytesPerChannel <= 0)) {
    return false;
  }

  constexpr std::uint32_t kExternalFormats[4] = {
      0x1903U, // GL_RED
      0x8227U, // GL_RG
      0x1907U, // GL_RGB
      0x1908U, // GL_RGBA
  };
  constexpr std::int32_t kHdrInternalFormats[4] = {
      0x822D, // GL_R16F
      0x822F, // GL_RG16F
      0x881B, // GL_RGB16F
      0x881A, // GL_RGBA16F
  };

  const std::size_t formatIndex = static_cast<std::size_t>(channels - 1);
  outLayout->externalFormat = kExternalFormats[formatIndex];
  outLayout->internalFormat =
      hdr ? kHdrInternalFormats[formatIndex]
          : static_cast<std::int32_t>(kExternalFormats[formatIndex]);

  const std::uint64_t rowBytes =
      static_cast<std::uint64_t>(width) *
      static_cast<std::uint64_t>(channels) *
      static_cast<std::uint64_t>(bytesPerChannel);
  if ((rowBytes % 8ULL) == 0ULL) {
    outLayout->unpackAlignment = 8;
  } else if ((rowBytes % 4ULL) == 0ULL) {
    outLayout->unpackAlignment = 4;
  } else if ((rowBytes % 2ULL) == 0ULL) {
    outLayout->unpackAlignment = 2;
  }
  return true;
}

/// Executes an upload while preserving the caller's unpack alignment.
template <typename GetAlignment, typename SetAlignment, typename Upload>
inline void with_gl_unpack_alignment(
    std::int32_t requiredAlignment, GetAlignment getAlignment,
    SetAlignment setAlignment, Upload upload) noexcept {
  std::int32_t previousAlignment = 4;
  getAlignment(&previousAlignment);
  const bool changed = previousAlignment != requiredAlignment;
  if (changed) {
    setAlignment(requiredAlignment);
  }
  upload();
  if (changed) {
    setAlignment(previousAlignment);
  }
}

} // namespace engine::renderer::detail
