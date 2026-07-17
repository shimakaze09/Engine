// Internal helpers shared by the inline math headers: SIMD availability
// detection and small scalar utilities. Not part of the public math API.

#pragma once

// SSE2 detection: MSVC x64 always has SSE2; GCC/Clang define __SSE2__.
#if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
#define ENGINE_MATH_SSE2 1
#include <emmintrin.h>
#else
#define ENGINE_MATH_SSE2 0
#endif

namespace engine::math::detail {

#if ENGINE_MATH_SSE2
/// Horizontal sum of all four lanes, returned as a scalar.
inline float sse2_hsum(__m128 v) noexcept {
  __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
  __m128 sums = _mm_add_ps(v, shuf);
  shuf = _mm_movehl_ps(shuf, sums);
  sums = _mm_add_ss(sums, shuf);
  return _mm_cvtss_f32(sums);
}
#endif

/// Clamps a scalar to [minValue, maxValue].
constexpr float clamp_scalar(float value, float minValue,
                             float maxValue) noexcept {
  if (value < minValue) {
    return minValue;
  }

  if (value > maxValue) {
    return maxValue;
  }

  return value;
}

} // namespace engine::math::detail
