// Declares the CPU downsample that fills a texture's mip chain: one level
// from the level above it, a box filter over the client texels (8-bit
// unsigned or 32-bit float components), for a backend that cannot generate
// the chain on the device.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Extent of mip level `level` of a dimension `extent`: halved per level,
/// never below 1.
std::int32_t mip_extent(std::int32_t extent, std::int32_t level) noexcept;

/// Bytes one client texel of `components` components takes.
std::size_t client_texel_bytes(TexelData data,
                               std::int32_t components) noexcept;

/// Writes the level below `src` (srcWidth x srcHeight texels of
/// `components` components) into `dst`, which holds
/// mip_extent(srcWidth, 1) x mip_extent(srcHeight, 1) texels. Each
/// destination texel averages the 2x2 block above it, and along an odd
/// dimension the last one also takes the extra source row or column, so
/// every source texel contributes; a 1-texel dimension stays one texel.
/// U8 averages round half up.
/// False, writing nothing, for a null buffer, a non-positive size or
/// components outside 1..4.
bool downsample_texels(TexelData data, std::int32_t components, const void *src,
                       std::int32_t srcWidth, std::int32_t srcHeight,
                       void *dst) noexcept;

} // namespace engine::renderer
