#include "engine/math/vec2.h"

#include <cmath>

namespace engine::math {

Vec2 add(const Vec2 &lhs, const Vec2 &rhs) noexcept {
  return Vec2(lhs.x + rhs.x, lhs.y + rhs.y);
}

Vec2 sub(const Vec2 &lhs, const Vec2 &rhs) noexcept {
  return Vec2(lhs.x - rhs.x, lhs.y - rhs.y);
}

Vec2 mul(const Vec2 &lhs, float scalar) noexcept {
  return Vec2(lhs.x * scalar, lhs.y * scalar);
}

Vec2 div(const Vec2 &lhs, float scalar) noexcept {
  const float inv = (scalar != 0.0F) ? (1.0F / scalar) : 0.0F;
  return Vec2(lhs.x * inv, lhs.y * inv);
}

float dot(const Vec2 &lhs, const Vec2 &rhs) noexcept {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y);
}

float length_sq(const Vec2 &value) noexcept { return dot(value, value); }

float length(const Vec2 &value) noexcept { return std::sqrt(length_sq(value)); }

Vec2 normalize(const Vec2 &value) noexcept {
  const float len = length(value);
  if (len <= 0.0F) {
    return Vec2();
  }

  return div(value, len);
}

Vec2 lerp(const Vec2 &from, const Vec2 &to, float t) noexcept {
  return add(from, mul(sub(to, from), t));
}

} // namespace engine::math
