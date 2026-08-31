// Column-major 4x4 float matrix with SSE2-accelerated multiply/transpose and
// a Gauss-Jordan inverse, defined inline for hot render/transform paths.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "engine/math/math_detail.h"
#include "engine/math/vec4.h"

namespace engine::math {

/// Column-major 4x4 float matrix; columns[i] is the i-th basis column.
struct Mat4 final {
  Vec4 columns[4];

  /// Identity matrix.
  constexpr Mat4() noexcept
      : columns{Vec4(1.0F, 0.0F, 0.0F, 0.0F), Vec4(0.0F, 1.0F, 0.0F, 0.0F),
                Vec4(0.0F, 0.0F, 1.0F, 0.0F), Vec4(0.0F, 0.0F, 0.0F, 1.0F)} {}

  /// Column-wise constructor.
  constexpr Mat4(const Vec4 &c0, const Vec4 &c1, const Vec4 &c2,
                 const Vec4 &c3) noexcept
      : columns{c0, c1, c2, c3} {}
};

static_assert(alignof(Mat4) == 16U, "Mat4 must stay 16-byte aligned.");

/// Identity matrix.
constexpr Mat4 identity() noexcept { return Mat4(); }

/// Matrix * column vector.
inline Vec4 mul(const Mat4 &lhs, const Vec4 &rhs) noexcept {
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
  return Vec4((lhs.columns[0].x * rhs.x) + (lhs.columns[1].x * rhs.y) +
                  (lhs.columns[2].x * rhs.z) + (lhs.columns[3].x * rhs.w),
              (lhs.columns[0].y * rhs.x) + (lhs.columns[1].y * rhs.y) +
                  (lhs.columns[2].y * rhs.z) + (lhs.columns[3].y * rhs.w),
              (lhs.columns[0].z * rhs.x) + (lhs.columns[1].z * rhs.y) +
                  (lhs.columns[2].z * rhs.z) + (lhs.columns[3].z * rhs.w),
              (lhs.columns[0].w * rhs.x) + (lhs.columns[1].w * rhs.y) +
                  (lhs.columns[2].w * rhs.z) + (lhs.columns[3].w * rhs.w));
#endif
}

/// Matrix * matrix (applies rhs first, then lhs).
inline Mat4 mul(const Mat4 &lhs, const Mat4 &rhs) noexcept {
  return Mat4(mul(lhs, rhs.columns[0]), mul(lhs, rhs.columns[1]),
              mul(lhs, rhs.columns[2]), mul(lhs, rhs.columns[3]));
}

/// Transposed copy.
inline Mat4 transpose(const Mat4 &value) noexcept {
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
  return Mat4(
      Vec4(value.columns[0].x, value.columns[1].x, value.columns[2].x,
           value.columns[3].x),
      Vec4(value.columns[0].y, value.columns[1].y, value.columns[2].y,
           value.columns[3].y),
      Vec4(value.columns[0].z, value.columns[1].z, value.columns[2].z,
           value.columns[3].z),
      Vec4(value.columns[0].w, value.columns[1].w, value.columns[2].w,
           value.columns[3].w));
#endif
}

/// Gauss-Jordan inverse with partial pivoting. Returns false — leaving
/// *out untouched — for non-finite input, an exactly singular pivot, or a
/// computed inverse that fails the residual gate below. Singularity is
/// judged by usability of the result rather than by any absolute pivot
/// magnitude, so a transform with a finite nonzero axis scale of any
/// magnitude (1e-7 as readily as 1e+7) inverts, while a matrix close
/// enough to singular that its float inverse would be garbage is refused.
/// The gate bounds max|M*inverse(M) - I| at 1e-3: orders of magnitude
/// above the float-epsilon-scale residual of well-conditioned transforms,
/// orders below the O(1)-and-up residual of a genuine float breakdown.
inline bool inverse(const Mat4 &value, Mat4 *out) noexcept {
  if (out == nullptr) {
    return false;
  }

  // A non-finite entry poisons every downstream consumer; reject it here,
  // where the contract is checkable, instead of letting NaN comparisons
  // steer the elimination.
  for (std::size_t col = 0U; col < 4U; ++col) {
    const Vec4 &c = value.columns[col];
    if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) ||
        !std::isfinite(c.w)) {
      return false;
    }
  }

  // Row-major copy of the input, kept for the residual gate at the end.
  const float src[4][4] = {
      {value.columns[0].x, value.columns[1].x, value.columns[2].x,
       value.columns[3].x},
      {value.columns[0].y, value.columns[1].y, value.columns[2].y,
       value.columns[3].y},
      {value.columns[0].z, value.columns[1].z, value.columns[2].z,
       value.columns[3].z},
      {value.columns[0].w, value.columns[1].w, value.columns[2].w,
       value.columns[3].w}};

  float m[4][8] = {{src[0][0], src[0][1], src[0][2], src[0][3],
                    1.0F, 0.0F, 0.0F, 0.0F},
                   {src[1][0], src[1][1], src[1][2], src[1][3],
                    0.0F, 1.0F, 0.0F, 0.0F},
                   {src[2][0], src[2][1], src[2][2], src[2][3],
                    0.0F, 0.0F, 1.0F, 0.0F},
                   {src[3][0], src[3][1], src[3][2], src[3][3],
                    0.0F, 0.0F, 0.0F, 1.0F}};

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

    // Only an exactly zero pivot is structural singularity; everything
    // between zero and "too ill-conditioned for float" is caught by the
    // residual gate on the finished inverse, which measures the actual
    // damage instead of guessing from one pivot's magnitude.
    if (pivot == 0.0F) {
      return false;
    }

    if (pivotRow != col) {
      for (std::size_t j = 0U; j < 8U; ++j) {
        const float temp = m[col][j];
        m[col][j] = m[pivotRow][j];
        m[pivotRow][j] = temp;
      }
    }

    // A denormal pivot overflows its reciprocal to infinity; that is the
    // same unusable-inverse outcome as a zero pivot, caught eagerly.
    const float invPivot = 1.0F / m[col][col];
    if (!std::isfinite(invPivot)) {
      return false;
    }
    for (std::size_t j = 0U; j < 8U; ++j) {
      m[col][j] *= invPivot;
    }

    for (std::size_t row = 0U; row < 4U; ++row) {
      if (row == col) {
        continue;
      }

      // Only an exact zero skips the elimination: a small-but-nonzero
      // factor still carries real coupling for rows whose own scale is
      // small, and dropping it would silently degrade their inverse.
      const float factor = m[row][col];
      if (factor == 0.0F) {
        continue;
      }

      for (std::size_t j = 0U; j < 8U; ++j) {
        m[row][j] -= factor * m[col][j];
      }
    }
  }

  for (std::size_t row = 0U; row < 4U; ++row) {
    for (std::size_t j = 4U; j < 8U; ++j) {
      if (!std::isfinite(m[row][j])) {
        return false;
      }
    }
  }

  // Residual gate: reject an inverse float precision could not actually
  // deliver. The residual of M*inverse(M) against I is normalized by the
  // magnitudes summed into each entry (a backward-error measure): entries
  // built from large operands — a big translation against a big scale —
  // legitimately carry rounding proportional to those operands, while a
  // genuine breakdown loses all significance and lands at the normalized
  // O(1) scale. The +1 floor keeps small-magnitude entries held to the
  // absolute bound. m[r][4 + c] holds inverse(M)[r][c].
  constexpr float kMaxResidual = 1.0e-3F;
  for (std::size_t row = 0U; row < 4U; ++row) {
    for (std::size_t col = 0U; col < 4U; ++col) {
      float product = 0.0F;
      float magnitude = 0.0F;
      for (std::size_t k = 0U; k < 4U; ++k) {
        const float term = src[row][k] * m[k][4U + col];
        product += term;
        magnitude += std::fabs(term);
      }
      const float expected = (row == col) ? 1.0F : 0.0F;
      if (!std::isfinite(magnitude) ||
          !(std::fabs(product - expected) <=
            (kMaxResidual * (magnitude + 1.0F)))) {
        return false;
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
