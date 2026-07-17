// 16-byte-aligned 4D float vector with SSE2-accelerated arithmetic, defined
// inline so hot callers pay no cross-module call cost.

#pragma once

#include <cmath>

#include "engine/math/math_detail.h"

namespace engine::math {

/// 4D float vector (x, y, z, w), 16-byte aligned for SIMD load/store.
struct alignas(16) Vec4 final {
  float x;
  float y;
  float z;
  float w;

  /// Zero vector.
  constexpr Vec4() noexcept : x(0.0F), y(0.0F), z(0.0F), w(0.0F) {}

  /// Component-wise constructor.
  constexpr Vec4(float xIn, float yIn, float zIn, float wIn) noexcept
      : x(xIn), y(yIn), z(zIn), w(wIn) {}
};

static_assert(alignof(Vec4) == 16U, "Vec4 must stay 16-byte aligned.");
static_assert(sizeof(Vec4) == 16U,
              "Vec4 must stay tightly packed for SIMD handoff.");

/// Component-wise sum.
inline Vec4 add(const Vec4 &lhs, const Vec4 &rhs) noexcept {
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&lhs.x);
  __m128 b = _mm_load_ps(&rhs.x);
  Vec4 result;
  _mm_store_ps(&result.x, _mm_add_ps(a, b));
  return result;
#else
  return Vec4(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, lhs.w + rhs.w);
#endif
}

/// Component-wise difference.
inline Vec4 sub(const Vec4 &lhs, const Vec4 &rhs) noexcept {
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&lhs.x);
  __m128 b = _mm_load_ps(&rhs.x);
  Vec4 result;
  _mm_store_ps(&result.x, _mm_sub_ps(a, b));
  return result;
#else
  return Vec4(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z, lhs.w - rhs.w);
#endif
}

/// Uniform scale.
inline Vec4 mul(const Vec4 &lhs, float scalar) noexcept {
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&lhs.x);
  __m128 s = _mm_set1_ps(scalar);
  Vec4 result;
  _mm_store_ps(&result.x, _mm_mul_ps(a, s));
  return result;
#else
  return Vec4(lhs.x * scalar, lhs.y * scalar, lhs.z * scalar, lhs.w * scalar);
#endif
}

/// Uniform inverse scale; division by zero yields the zero vector.
inline Vec4 div(const Vec4 &lhs, float scalar) noexcept {
  const float inv = (scalar != 0.0F) ? (1.0F / scalar) : 0.0F;
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&lhs.x);
  __m128 s = _mm_set1_ps(inv);
  Vec4 result;
  _mm_store_ps(&result.x, _mm_mul_ps(a, s));
  return result;
#else
  return Vec4(lhs.x * inv, lhs.y * inv, lhs.z * inv, lhs.w * inv);
#endif
}

/// Dot product.
inline float dot(const Vec4 &lhs, const Vec4 &rhs) noexcept {
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&lhs.x);
  __m128 b = _mm_load_ps(&rhs.x);
  return detail::sse2_hsum(_mm_mul_ps(a, b));
#else
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z) + (lhs.w * rhs.w);
#endif
}

/// Squared length (avoids the sqrt when only comparing magnitudes).
inline float length_sq(const Vec4 &value) noexcept {
  return dot(value, value);
}

/// Euclidean length.
inline float length(const Vec4 &value) noexcept {
  return std::sqrt(length_sq(value));
}

/// Unit-length copy; the zero vector normalizes to zero.
inline Vec4 normalize(const Vec4 &value) noexcept {
  const float len = length(value);
  if (len <= 0.0F) {
    return Vec4();
  }

  return div(value, len);
}

/// Linear interpolation: from + (to - from) * t.
inline Vec4 lerp(const Vec4 &from, const Vec4 &to, float t) noexcept {
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&from.x);
  __m128 b = _mm_load_ps(&to.x);
  __m128 tt = _mm_set1_ps(t);
  Vec4 result;
  _mm_store_ps(&result.x, _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), tt)));
  return result;
#else
  return add(from, mul(sub(to, from), t));
#endif
}

} // namespace engine::math
