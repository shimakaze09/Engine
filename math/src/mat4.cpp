#include "engine/math/mat4.h"

#include <cmath>

namespace engine::math {

Mat4 identity() noexcept { return Mat4(); }

Vec4 mul(const Mat4 &lhs, const Vec4 &rhs) noexcept {
  return Vec4((lhs.columns[0].x * rhs.x) + (lhs.columns[1].x * rhs.y) +
                  (lhs.columns[2].x * rhs.z) + (lhs.columns[3].x * rhs.w),
              (lhs.columns[0].y * rhs.x) + (lhs.columns[1].y * rhs.y) +
                  (lhs.columns[2].y * rhs.z) + (lhs.columns[3].y * rhs.w),
              (lhs.columns[0].z * rhs.x) + (lhs.columns[1].z * rhs.y) +
                  (lhs.columns[2].z * rhs.z) + (lhs.columns[3].z * rhs.w),
              (lhs.columns[0].w * rhs.x) + (lhs.columns[1].w * rhs.y) +
                  (lhs.columns[2].w * rhs.z) + (lhs.columns[3].w * rhs.w));
}

Mat4 mul(const Mat4 &lhs, const Mat4 &rhs) noexcept {
  return Mat4(mul(lhs, rhs.columns[0]), mul(lhs, rhs.columns[1]),
              mul(lhs, rhs.columns[2]), mul(lhs, rhs.columns[3]));
}

Mat4 transpose(const Mat4 &value) noexcept {
  return Mat4(Vec4(value.columns[0].x, value.columns[1].x, value.columns[2].x,
                   value.columns[3].x),
              Vec4(value.columns[0].y, value.columns[1].y, value.columns[2].y,
                   value.columns[3].y),
              Vec4(value.columns[0].z, value.columns[1].z, value.columns[2].z,
                   value.columns[3].z),
              Vec4(value.columns[0].w, value.columns[1].w, value.columns[2].w,
                   value.columns[3].w));
}

bool inverse(const Mat4 &value, Mat4 *out) noexcept {
  if (out == nullptr) {
    return false;
  }

  float m[4][8] = {{value.columns[0].x, value.columns[1].x, value.columns[2].x,
                    value.columns[3].x, 1.0F, 0.0F, 0.0F, 0.0F},
                   {value.columns[0].y, value.columns[1].y, value.columns[2].y,
                    value.columns[3].y, 0.0F, 1.0F, 0.0F, 0.0F},
                   {value.columns[0].z, value.columns[1].z, value.columns[2].z,
                    value.columns[3].z, 0.0F, 0.0F, 1.0F, 0.0F},
                   {value.columns[0].w, value.columns[1].w, value.columns[2].w,
                    value.columns[3].w, 0.0F, 0.0F, 0.0F, 1.0F}};

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
