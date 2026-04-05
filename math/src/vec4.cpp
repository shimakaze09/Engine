#include "engine/math/vec4.h"

#include <cmath>

namespace engine::math {

Vec4 add(const Vec4 &lhs, const Vec4 &rhs) noexcept {
  return Vec4(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, lhs.w + rhs.w);
}

Vec4 sub(const Vec4 &lhs, const Vec4 &rhs) noexcept {
  return Vec4(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z, lhs.w - rhs.w);
}

Vec4 mul(const Vec4 &lhs, float scalar) noexcept {
  return Vec4(lhs.x * scalar, lhs.y * scalar, lhs.z * scalar, lhs.w * scalar);
}

Vec4 div(const Vec4 &lhs, float scalar) noexcept {
  const float inv = (scalar != 0.0F) ? (1.0F / scalar) : 0.0F;
  return Vec4(lhs.x * inv, lhs.y * inv, lhs.z * inv, lhs.w * inv);
}

float dot(const Vec4 &lhs, const Vec4 &rhs) noexcept {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z) + (lhs.w * rhs.w);
}

float length_sq(const Vec4 &value) noexcept { return dot(value, value); }

float length(const Vec4 &value) noexcept { return std::sqrt(length_sq(value)); }

Vec4 normalize(const Vec4 &value) noexcept {
  const float len = length(value);
  if (len <= 0.0F) {
    return Vec4();
  }

  return div(value, len);
}

Vec4 lerp(const Vec4 &from, const Vec4 &to, float t) noexcept {
  return add(from, mul(sub(to, from), t));
}

} // namespace engine::math
