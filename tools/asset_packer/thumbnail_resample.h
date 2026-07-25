// Declares deterministic image resampling used by asset thumbnails.

#pragma once

#include <cstdint>

namespace engine::tools {

/// Resizes tightly packed RGBA8 pixels with clamped bilinear sampling.
[[nodiscard]] bool
resize_rgba_bilinear(const std::uint8_t *source, int sourceWidth,
                     int sourceHeight, std::uint8_t *destination,
                     int destinationWidth, int destinationHeight) noexcept;

} // namespace engine::tools
