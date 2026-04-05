#pragma once

namespace engine::math {

struct alignas(16) Vec4 final {
  float x;
  float y;
  float z;
  float w;

  constexpr Vec4() noexcept : x(0.0F), y(0.0F), z(0.0F), w(0.0F) {}

  constexpr Vec4(float xIn, float yIn, float zIn, float wIn) noexcept
      : x(xIn), y(yIn), z(zIn), w(wIn) {}
};

static_assert(alignof(Vec4) == 16U, "Vec4 must stay 16-byte aligned.");
static_assert(sizeof(Vec4) == 16U,
              "Vec4 must stay tightly packed for SIMD handoff.");

Vec4 add(const Vec4 &lhs, const Vec4 &rhs) noexcept;
Vec4 sub(const Vec4 &lhs, const Vec4 &rhs) noexcept;
Vec4 mul(const Vec4 &lhs, float scalar) noexcept;
Vec4 div(const Vec4 &lhs, float scalar) noexcept;
float dot(const Vec4 &lhs, const Vec4 &rhs) noexcept;
float length_sq(const Vec4 &value) noexcept;
float length(const Vec4 &value) noexcept;
Vec4 normalize(const Vec4 &value) noexcept;
Vec4 lerp(const Vec4 &from, const Vec4 &to, float t) noexcept;

} // namespace engine::math
