#include "engine/math/vec4.h"

#include <cmath>

// SSE2 detection: MSVC x64 always has SSE2; GCC/Clang define __SSE2__.
#if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
#define ENGINE_MATH_SSE2 1
#include <emmintrin.h>
#else
#define ENGINE_MATH_SSE2 0
#endif

namespace engine::math {

Vec4 add(const Vec4 &lhs, const Vec4 &rhs) noexcept {
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

Vec4 sub(const Vec4 &lhs, const Vec4 &rhs) noexcept {
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

Vec4 mul(const Vec4 &lhs, float scalar) noexcept {
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

Vec4 div(const Vec4 &lhs, float scalar) noexcept {
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

float dot(const Vec4 &lhs, const Vec4 &rhs) noexcept {
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&lhs.x);
  __m128 b = _mm_load_ps(&rhs.x);
  __m128 prod = _mm_mul_ps(a, b);
  // Horizontal sum: prod = (x*x, y*y, z*z, w*w)
  __m128 shuf = _mm_shuffle_ps(prod, prod, _MM_SHUFFLE(2, 3, 0, 1));
  __m128 sums = _mm_add_ps(prod, shuf); // (x+y, y+x, z+w, w+z)
  shuf = _mm_movehl_ps(shuf, sums);     // (z+w, w+z, ...)
  sums = _mm_add_ss(sums, shuf);        // (x+y+z+w, ...)
  return _mm_cvtss_f32(sums);
#else
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z) + (lhs.w * rhs.w);
#endif
}

float length_sq(const Vec4 &value) noexcept {
  return dot(value, value);
}

float length(const Vec4 &value) noexcept {
  return std::sqrt(length_sq(value));
}

Vec4 normalize(const Vec4 &value) noexcept {
  const float len = length(value);
  if (len <= 0.0F) {
    return Vec4();
  }

  return div(value, len);
}

Vec4 lerp(const Vec4 &from, const Vec4 &to, float t) noexcept {
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&from.x);
  __m128 b = _mm_load_ps(&to.x);
  __m128 tt = _mm_set1_ps(t);
  // from + (to - from) * t
  Vec4 result;
  _mm_store_ps(&result.x, _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), tt)));
  return result;
#else
  return add(from, mul(sub(to, from), t));
#endif
}

} // namespace engine::math
