#pragma once

#include "engine/math/vec4.h"

namespace engine::math {

struct Mat4 final {
  Vec4 columns[4];

  constexpr Mat4() noexcept
      : columns{Vec4(1.0F, 0.0F, 0.0F, 0.0F), Vec4(0.0F, 1.0F, 0.0F, 0.0F),
                Vec4(0.0F, 0.0F, 1.0F, 0.0F), Vec4(0.0F, 0.0F, 0.0F, 1.0F)} {}

  constexpr Mat4(const Vec4 &c0, const Vec4 &c1, const Vec4 &c2,
                 const Vec4 &c3) noexcept
      : columns{c0, c1, c2, c3} {}
};

static_assert(alignof(Mat4) == 16U, "Mat4 must stay 16-byte aligned.");

Mat4 identity() noexcept;
Mat4 transpose(const Mat4 &value) noexcept;
Mat4 mul(const Mat4 &lhs, const Mat4 &rhs) noexcept;
Vec4 mul(const Mat4 &lhs, const Vec4 &rhs) noexcept;
bool inverse(const Mat4 &value, Mat4 *out) noexcept;

} // namespace engine::math
