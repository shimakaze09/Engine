// Implements the CPU box-filter downsample that fills a texture's mip
// chain one level at a time.

#include "texel_downsample.h"

#include <array>
#include <cmath>

#include "engine/math/color_space.h"

namespace engine::renderer {

std::int32_t mip_extent(std::int32_t extent, std::int32_t level) noexcept {
  std::int32_t result = extent;
  for (std::int32_t i = 0; (i < level) && (result > 1); ++i) {
    result /= 2;
  }
  return (result > 0) ? result : 1;
}

std::size_t client_texel_bytes(TexelData data,
                               std::int32_t components) noexcept {
  const std::size_t componentBytes = (data == TexelData::F32) ? 4U : 1U;
  return componentBytes * static_cast<std::size_t>(components);
}

namespace {

/// The source texels destination texel `index` covers along one axis: the
/// pair 2*index, 2*index+1, plus the extra last texel of an odd extent in
/// the final destination texel, so every source texel contributes once.
/// A 1-texel extent covers its one texel.
struct Taps final {
  std::int32_t first = 0;
  std::int32_t count = 1;
};

Taps taps_for(std::int32_t index, std::int32_t srcExtent,
              std::int32_t dstExtent) noexcept {
  if (srcExtent <= 1) {
    return Taps{0, 1};
  }
  const bool oddTail = ((srcExtent % 2) != 0) && (index == dstExtent - 1);
  return Taps{index * 2, oddTail ? 3 : 2};
}

/// Averages the source texels each destination texel covers.
/// `Component` is the client component type and `Sum` the type the block
/// is summed in.
template <typename Component, typename Sum>
void downsample(const Component *src, std::int32_t srcWidth,
                std::int32_t srcHeight, std::int32_t components,
                Component *dst) noexcept {
  const std::int32_t dstWidth = mip_extent(srcWidth, 1);
  const std::int32_t dstHeight = mip_extent(srcHeight, 1);
  const auto stride = static_cast<std::size_t>(components);
  for (std::int32_t y = 0; y < dstHeight; ++y) {
    const Taps rows = taps_for(y, srcHeight, dstHeight);
    for (std::int32_t x = 0; x < dstWidth; ++x) {
      const Taps columns = taps_for(x, srcWidth, dstWidth);
      const Sum taps = static_cast<Sum>(rows.count * columns.count);
      for (std::int32_t c = 0; c < components; ++c) {
        Sum sum = 0;
        for (std::int32_t sy = rows.first; sy < rows.first + rows.count; ++sy) {
          for (std::int32_t sx = columns.first;
               sx < columns.first + columns.count; ++sx) {
            sum += static_cast<Sum>(src[((static_cast<std::size_t>(sy) *
                                          static_cast<std::size_t>(srcWidth)) +
                                         static_cast<std::size_t>(sx)) *
                                            stride +
                                        static_cast<std::size_t>(c)]);
          }
        }
        Component value{};
        if constexpr (sizeof(Component) == 1U) {
          value = static_cast<Component>((sum + (taps / 2U)) / taps);
        } else {
          value = static_cast<Component>(sum / taps);
        }
        dst[((static_cast<std::size_t>(y) *
              static_cast<std::size_t>(dstWidth)) +
             static_cast<std::size_t>(x)) *
                stride +
            static_cast<std::size_t>(c)] = value;
      }
    }
  }
}

/// The linear value of every 8-bit sRGB code, built once.
const std::array<float, 256U> &srgb_decode_table() noexcept {
  static const std::array<float, 256U> table = []() noexcept {
    std::array<float, 256U> values{};
    for (std::size_t i = 0U; i < values.size(); ++i) {
      values[i] = static_cast<float>(
          math::srgb_to_linear(static_cast<double>(i) / 255.0));
    }
    return values;
  }();
  return table;
}

/// Averages each destination texel's taps in linear light for the colour
/// components, and as bytes for alpha.
void downsample_srgb(const std::uint8_t *src, std::int32_t srcWidth,
                     std::int32_t srcHeight, std::int32_t components,
                     std::uint8_t *dst) noexcept {
  const std::array<float, 256U> &decode = srgb_decode_table();
  const std::int32_t dstWidth = mip_extent(srcWidth, 1);
  const std::int32_t dstHeight = mip_extent(srcHeight, 1);
  const auto stride = static_cast<std::size_t>(components);
  const std::int32_t colourComponents = (components == 4) ? 3 : components;
  for (std::int32_t y = 0; y < dstHeight; ++y) {
    const Taps rows = taps_for(y, srcHeight, dstHeight);
    for (std::int32_t x = 0; x < dstWidth; ++x) {
      const Taps columns = taps_for(x, srcWidth, dstWidth);
      const auto taps = static_cast<std::uint32_t>(rows.count * columns.count);
      for (std::int32_t c = 0; c < components; ++c) {
        float linearSum = 0.0F;
        std::uint32_t byteSum = 0U;
        for (std::int32_t sy = rows.first; sy < rows.first + rows.count; ++sy) {
          for (std::int32_t sx = columns.first;
               sx < columns.first + columns.count; ++sx) {
            const std::uint8_t texel =
                src[((static_cast<std::size_t>(sy) *
                      static_cast<std::size_t>(srcWidth)) +
                     static_cast<std::size_t>(sx)) *
                        stride +
                    static_cast<std::size_t>(c)];
            linearSum += decode[texel];
            byteSum += texel;
          }
        }
        dst[((static_cast<std::size_t>(y) *
              static_cast<std::size_t>(dstWidth)) +
             static_cast<std::size_t>(x)) *
                stride +
            static_cast<std::size_t>(c)] =
            (c < colourComponents)
                ? linear_to_srgb_byte(linearSum / static_cast<float>(taps))
                : static_cast<std::uint8_t>((byteSum + (taps / 2U)) / taps);
      }
    }
  }
}

} // namespace

float srgb_byte_to_linear(std::uint8_t encoded) noexcept {
  return srgb_decode_table()[encoded];
}

std::uint8_t linear_to_srgb_byte(float linear) noexcept {
  // The nearest code by linear value: a binary search of the decode
  // table, then the closer of the two neighbours.
  const std::array<float, 256U> &decode = srgb_decode_table();
  std::size_t low = 0U;
  std::size_t high = decode.size() - 1U;
  if (!(linear > decode[low])) {
    return 0U;
  }
  if (linear >= decode[high]) {
    return 255U;
  }
  while (high - low > 1U) {
    const std::size_t mid = (low + high) / 2U;
    if (decode[mid] <= linear) {
      low = mid;
    } else {
      high = mid;
    }
  }
  return static_cast<std::uint8_t>(
      ((linear - decode[low]) <= (decode[high] - linear)) ? low : high);
}

bool downsample_texels(TexelData data, std::int32_t components, const void *src,
                       std::int32_t srcWidth, std::int32_t srcHeight, void *dst,
                       bool srgb) noexcept {
  if ((src == nullptr) || (dst == nullptr) || (srcWidth <= 0) ||
      (srcHeight <= 0) || (components < 1) || (components > 4)) {
    return false;
  }
  if (srgb && (data == TexelData::U8)) {
    downsample_srgb(static_cast<const std::uint8_t *>(src), srcWidth, srcHeight,
                    components, static_cast<std::uint8_t *>(dst));
    return true;
  }
  if (data == TexelData::F32) {
    downsample<float, float>(static_cast<const float *>(src), srcWidth,
                             srcHeight, components, static_cast<float *>(dst));
  } else {
    downsample<std::uint8_t, std::uint32_t>(
        static_cast<const std::uint8_t *>(src), srcWidth, srcHeight, components,
        static_cast<std::uint8_t *>(dst));
  }
  return true;
}

} // namespace engine::renderer
