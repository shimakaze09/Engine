#include "engine/math/mat4.h"

#include <cmath>

#if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
#define ENGINE_MATH_SSE2 1
#include <emmintrin.h>
#else
#define ENGINE_MATH_SSE2 0
#endif

namespace engine::math {

Mat4 identity() noexcept {
  return Mat4();
}

Vec4 mul(const Mat4 &lhs, const Vec4 &rhs) noexcept {
#if ENGINE_MATH_SSE2
  __m128 c0 = _mm_load_ps(&lhs.columns[0].x);
  __m128 c1 = _mm_load_ps(&lhs.columns[1].x);
  __m128 c2 = _mm_load_ps(&lhs.columns[2].x);
  __m128 c3 = _mm_load_ps(&lhs.columns[3].x);

  __m128 vx = _mm_set1_ps(rhs.x);
  __m128 vy = _mm_set1_ps(rhs.y);
  __m128 vz = _mm_set1_ps(rhs.z);
  __m128 vw = _mm_set1_ps(rhs.w);

  __m128 result =
      _mm_add_ps(_mm_add_ps(_mm_mul_ps(c0, vx), _mm_mul_ps(c1, vy)),
                 _mm_add_ps(_mm_mul_ps(c2, vz), _mm_mul_ps(c3, vw)));

  Vec4 out;
  _mm_store_ps(&out.x, result);
  return out;
#else
  return Vec4((lhs.columns[0].x * rhs.x) + (lhs.columns[1].x * rhs.y)
                  + (lhs.columns[2].x * rhs.z) + (lhs.columns[3].x * rhs.w),
              (lhs.columns[0].y * rhs.x) + (lhs.columns[1].y * rhs.y)
                  + (lhs.columns[2].y * rhs.z) + (lhs.columns[3].y * rhs.w),
              (lhs.columns[0].z * rhs.x) + (lhs.columns[1].z * rhs.y)
                  + (lhs.columns[2].z * rhs.z) + (lhs.columns[3].z * rhs.w),
              (lhs.columns[0].w * rhs.x) + (lhs.columns[1].w * rhs.y)
                  + (lhs.columns[2].w * rhs.z) + (lhs.columns[3].w * rhs.w));
#endif
}

Mat4 mul(const Mat4 &lhs, const Mat4 &rhs) noexcept {
  return Mat4(mul(lhs, rhs.columns[0]),
              mul(lhs, rhs.columns[1]),
              mul(lhs, rhs.columns[2]),
              mul(lhs, rhs.columns[3]));
}

Mat4 transpose(const Mat4 &value) noexcept {
#if ENGINE_MATH_SSE2
  __m128 c0 = _mm_load_ps(&value.columns[0].x);
  __m128 c1 = _mm_load_ps(&value.columns[1].x);
  __m128 c2 = _mm_load_ps(&value.columns[2].x);
  __m128 c3 = _mm_load_ps(&value.columns[3].x);

  // Interleave low/high pairs then shuffle.
  __m128 t0 = _mm_unpacklo_ps(c0, c1); // (c0.x, c1.x, c0.y, c1.y)
  __m128 t1 = _mm_unpackhi_ps(c0, c1); // (c0.z, c1.z, c0.w, c1.w)
  __m128 t2 = _mm_unpacklo_ps(c2, c3);
  __m128 t3 = _mm_unpackhi_ps(c2, c3);

  Mat4 out;
  _mm_store_ps(&out.columns[0].x, _mm_movelh_ps(t0, t2));
  _mm_store_ps(&out.columns[1].x, _mm_movehl_ps(t2, t0));
  _mm_store_ps(&out.columns[2].x, _mm_movelh_ps(t1, t3));
  _mm_store_ps(&out.columns[3].x, _mm_movehl_ps(t3, t1));
  return out;
#else
  return Mat4(Vec4(value.columns[0].x,
                   value.columns[1].x,
                   value.columns[2].x,
                   value.columns[3].x),
              Vec4(value.columns[0].y,
                   value.columns[1].y,
                   value.columns[2].y,
                   value.columns[3].y),
              Vec4(value.columns[0].z,
                   value.columns[1].z,
                   value.columns[2].z,
                   value.columns[3].z),
              Vec4(value.columns[0].w,
                   value.columns[1].w,
                   value.columns[2].w,
                   value.columns[3].w));
#endif
}

bool inverse(const Mat4 &value, Mat4 *out) noexcept {
  if (out == nullptr) {
    return false;
  }

  float m[4][8] = {{value.columns[0].x,
                    value.columns[1].x,
                    value.columns[2].x,
                    value.columns[3].x,
                    1.0F,
                    0.0F,
                    0.0F,
                    0.0F},
                   {value.columns[0].y,
                    value.columns[1].y,
                    value.columns[2].y,
                    value.columns[3].y,
                    0.0F,
                    1.0F,
                    0.0F,
                    0.0F},
                   {value.columns[0].z,
                    value.columns[1].z,
                    value.columns[2].z,
                    value.columns[3].z,
                    0.0F,
                    0.0F,
                    1.0F,
                    0.0F},
                   {value.columns[0].w,
                    value.columns[1].w,
                    value.columns[2].w,
                    value.columns[3].w,
                    0.0F,
                    0.0F,
                    0.0F,
                    1.0F}};

  constexpr float kEpsilon = 1.0e-6F;

  for (std::size_t col = 0U; col < 4U; ++col) {
    std::size_t pivotRow = col;
    float pivot = std::fabs(m[col][col]);

    for (std::size_t row = col + 1U; row < 4U; ++row) {
      const float candidate = std::fabs(m[row][col]);
      if (candidate > pivot) {
        pivot = candidate;
        pivotRow = row;
      }
    }

    if (pivot <= kEpsilon) {
      return false;
    }

    if (pivotRow != col) {
      for (std::size_t j = 0U; j < 8U; ++j) {
        const float temp = m[col][j];
        m[col][j] = m[pivotRow][j];
        m[pivotRow][j] = temp;
      }
    }

    const float invPivot = 1.0F / m[col][col];
    for (std::size_t j = 0U; j < 8U; ++j) {
      m[col][j] *= invPivot;
    }

    for (std::size_t row = 0U; row < 4U; ++row) {
      if (row == col) {
        continue;
      }

      const float factor = m[row][col];
      if (std::fabs(factor) <= kEpsilon) {
        continue;
      }

      for (std::size_t j = 0U; j < 8U; ++j) {
        m[row][j] -= factor * m[col][j];
      }
    }
  }

  *out = Mat4(Vec4(m[0][4], m[1][4], m[2][4], m[3][4]),
              Vec4(m[0][5], m[1][5], m[2][5], m[3][5]),
              Vec4(m[0][6], m[1][6], m[2][6], m[3][6]),
              Vec4(m[0][7], m[1][7], m[2][7], m[3][7]));
  return true;
}

} // namespace engine::math
