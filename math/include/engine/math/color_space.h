// The sRGB transfer function (IEC 61966-2-1) between linear light and its
// display encoding, one channel at a time. The renderer decodes sRGB
// texels with it, and the editor shows and picks linear colours through
// it, so a swatch shows the colour the renderer draws. Computed in double
// precision; not part of the deterministic simulation set (scalar.h),
// since no simulated state passes through it.

#pragma once

#include <cmath>

namespace engine::math {

/// The linear value of an sRGB-encoded channel. Values above 1 (HDR) and
/// below 0 continue the curve's two segments, so the mapping is monotonic
/// and inverts linear_to_srgb over the whole range.
[[nodiscard]] inline double srgb_to_linear(double encoded) noexcept {
  return (encoded <= 0.04045) ? (encoded / 12.92)
                              : std::pow((encoded + 0.055) / 1.055, 2.4);
}

/// The sRGB encoding of a linear channel; the inverse of srgb_to_linear.
[[nodiscard]] inline double linear_to_srgb(double linear) noexcept {
  return (linear <= 0.0031308)
             ? (linear * 12.92)
             : ((1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055);
}

/// srgb_to_linear for a float channel, computed in double.
[[nodiscard]] inline float srgb_to_linear(float encoded) noexcept {
  return static_cast<float>(srgb_to_linear(static_cast<double>(encoded)));
}

/// linear_to_srgb for a float channel, computed in double.
[[nodiscard]] inline float linear_to_srgb(float linear) noexcept {
  return static_cast<float>(linear_to_srgb(static_cast<double>(linear)));
}

} // namespace engine::math
