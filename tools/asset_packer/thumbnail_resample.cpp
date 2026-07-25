// Implements boundary-safe bilinear resampling for generated thumbnails.

#include "thumbnail_resample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace engine::tools {

bool resize_rgba_bilinear(const std::uint8_t *source, int sourceWidth,
                          int sourceHeight, std::uint8_t *destination,
                          int destinationWidth,
                          int destinationHeight) noexcept {
  if ((source == nullptr) || (destination == nullptr) || (sourceWidth <= 0) ||
      (sourceHeight <= 0) || (destinationWidth <= 0) ||
      (destinationHeight <= 0)) {
    return false;
  }

  constexpr std::size_t kChannels = 4U;
  const double maximumX = static_cast<double>(sourceWidth - 1);
  const double maximumY = static_cast<double>(sourceHeight - 1);

  for (int y = 0; y < destinationHeight; ++y) {
    const double unboundedY =
        ((static_cast<double>(y) + 0.5) /
         static_cast<double>(destinationHeight)) *
            static_cast<double>(sourceHeight) -
        0.5;
    const double sourceY = std::clamp(unboundedY, 0.0, maximumY);
    const int y0 = static_cast<int>(std::floor(sourceY));
    const int y1 = std::min(y0 + 1, sourceHeight - 1);
    const double weightY = sourceY - static_cast<double>(y0);

    for (int x = 0; x < destinationWidth; ++x) {
      const double unboundedX =
          ((static_cast<double>(x) + 0.5) /
           static_cast<double>(destinationWidth)) *
              static_cast<double>(sourceWidth) -
          0.5;
      const double sourceX = std::clamp(unboundedX, 0.0, maximumX);
      const int x0 = static_cast<int>(std::floor(sourceX));
      const int x1 = std::min(x0 + 1, sourceWidth - 1);
      const double weightX = sourceX - static_cast<double>(x0);

      const std::size_t source00 =
          (static_cast<std::size_t>(y0) *
               static_cast<std::size_t>(sourceWidth) +
           static_cast<std::size_t>(x0)) *
          kChannels;
      const std::size_t source10 =
          (static_cast<std::size_t>(y0) *
               static_cast<std::size_t>(sourceWidth) +
           static_cast<std::size_t>(x1)) *
          kChannels;
      const std::size_t source01 =
          (static_cast<std::size_t>(y1) *
               static_cast<std::size_t>(sourceWidth) +
           static_cast<std::size_t>(x0)) *
          kChannels;
      const std::size_t source11 =
          (static_cast<std::size_t>(y1) *
               static_cast<std::size_t>(sourceWidth) +
           static_cast<std::size_t>(x1)) *
          kChannels;
      const std::size_t output =
          (static_cast<std::size_t>(y) *
               static_cast<std::size_t>(destinationWidth) +
           static_cast<std::size_t>(x)) *
          kChannels;

      for (std::size_t channel = 0U; channel < kChannels; ++channel) {
        const double top =
            static_cast<double>(source[source00 + channel]) * (1.0 - weightX) +
            static_cast<double>(source[source10 + channel]) * weightX;
        const double bottom =
            static_cast<double>(source[source01 + channel]) * (1.0 - weightX) +
            static_cast<double>(source[source11 + channel]) * weightX;
        const double value = top * (1.0 - weightY) + bottom * weightY;
        destination[output + channel] =
            static_cast<std::uint8_t>(std::floor(value + 0.5));
      }
    }
  }
  return true;
}

} // namespace engine::tools
