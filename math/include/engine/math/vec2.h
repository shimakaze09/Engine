// 2D float vector and its arithmetic, defined inline so hot callers pay no
// cross-module call cost.

#pragma once

#include <cmath>

namespace engine::math {

/// 2D float vector (x, y).
struct Vec2 final {
  float x;
  float y;

  /// Zero vector.
  constexpr Vec2() noexcept : x(0.0F), y(0.0F) {}

  /// Component-wise constructor.
  constexpr Vec2(float xIn, float yIn) noexcept : x(xIn), y(yIn) {}
};

/// Component-wise sum.
constexpr Vec2 add(const Vec2 &lhs, const Vec2 &rhs) noexcept {
  return Vec2(lhs.x + rhs.x, lhs.y + rhs.y);
}

/// Component-wise difference.
constexpr Vec2 sub(const Vec2 &lhs, const Vec2 &rhs) noexcept {
  return Vec2(lhs.x - rhs.x, lhs.y - rhs.y);
}

/// Uniform scale.
constexpr Vec2 mul(const Vec2 &lhs, float scalar) noexcept {
  return Vec2(lhs.x * scalar, lhs.y * scalar);
}

/// Uniform inverse scale; division by zero yields the zero vector.
constexpr Vec2 div(const Vec2 &lhs, float scalar) noexcept {
  const float inv = (scalar != 0.0F) ? (1.0F / scalar) : 0.0F;
  return Vec2(lhs.x * inv, lhs.y * inv);
}

/// Dot product.
constexpr float dot(const Vec2 &lhs, const Vec2 &rhs) noexcept {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y);
}

/// Squared length (avoids the sqrt when only comparing magnitudes).
constexpr float length_sq(const Vec2 &value) noexcept {
  return dot(value, value);
}

/// Euclidean length.
inline float length(const Vec2 &value) noexcept {
  return std::sqrt(length_sq(value));
}

/// Unit-length copy; the zero vector normalizes to zero.
inline Vec2 normalize(const Vec2 &value) noexcept {
  const float len = length(value);
  if (len <= 0.0F) {
    return Vec2();
  }

  return div(value, len);
}

/// Linear interpolation: from + (to - from) * t.
constexpr Vec2 lerp(const Vec2 &from, const Vec2 &to, float t) noexcept {
  return add(from, mul(sub(to, from), t));
}

} // namespace engine::math
