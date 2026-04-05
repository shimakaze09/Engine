#pragma once

namespace engine::math {

struct Vec2 final {
  float x;
  float y;

  constexpr Vec2() noexcept : x(0.0F), y(0.0F) {}

  constexpr Vec2(float xIn, float yIn) noexcept : x(xIn), y(yIn) {}
};

Vec2 add(const Vec2 &lhs, const Vec2 &rhs) noexcept;
Vec2 sub(const Vec2 &lhs, const Vec2 &rhs) noexcept;
Vec2 mul(const Vec2 &lhs, float scalar) noexcept;
Vec2 div(const Vec2 &lhs, float scalar) noexcept;
float dot(const Vec2 &lhs, const Vec2 &rhs) noexcept;
float length_sq(const Vec2 &value) noexcept;
float length(const Vec2 &value) noexcept;
Vec2 normalize(const Vec2 &value) noexcept;
Vec2 lerp(const Vec2 &from, const Vec2 &to, float t) noexcept;

} // namespace engine::math
