// Implements vec3 behavior for the Engine math library.

#include "engine/math/vec3.h"

#include <cmath>

namespace engine::math {

/// Adds a value or component to the target system.
Vec3 add(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return Vec3(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z);
}

/// Handles sub.
Vec3 sub(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return Vec3(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z);
}

/// Handles mul.
Vec3 mul(const Vec3 &lhs, float scalar) noexcept {
  return Vec3(lhs.x * scalar, lhs.y * scalar, lhs.z * scalar);
}

/// Handles div.
Vec3 div(const Vec3 &lhs, float scalar) noexcept {
  const float inv = (scalar != 0.0F) ? (1.0F / scalar) : 0.0F;
  return Vec3(lhs.x * inv, lhs.y * inv, lhs.z * inv);
}

/// Handles dot.
float dot(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

/// Handles cross.
Vec3 cross(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return Vec3((lhs.y * rhs.z) - (lhs.z * rhs.y),
              (lhs.z * rhs.x) - (lhs.x * rhs.z),
              (lhs.x * rhs.y) - (lhs.y * rhs.x));
}

/// Handles length sq.
float length_sq(const Vec3 &value) noexcept { return dot(value, value); }

/// Handles length.
float length(const Vec3 &value) noexcept { return std::sqrt(length_sq(value)); }

/// Clamps and fills settings into a safe runtime range.
Vec3 normalize(const Vec3 &value) noexcept {
  const float len = length(value);
  if (len <= 0.0F) {
    return Vec3();
  }

  return div(value, len);
}

/// Handles lerp.
Vec3 lerp(const Vec3 &from, const Vec3 &to, float t) noexcept {
  return add(from, mul(sub(to, from), t));
}

/// Handles negate.
Vec3 negate(const Vec3 &value) noexcept {
  return Vec3(-value.x, -value.y, -value.z);
}

/// Handles reflect.
Vec3 reflect(const Vec3 &incident, const Vec3 &normal) noexcept {
  const float d = 2.0F * dot(incident, normal);
  return sub(incident, mul(normal, d));
}

/// Handles clamp.
Vec3 clamp(const Vec3 &value, float minVal, float maxVal) noexcept {
  const float cx = (value.x < minVal) ? minVal : ((value.x > maxVal) ? maxVal : value.x);
  const float cy = (value.y < minVal) ? minVal : ((value.y > maxVal) ? maxVal : value.y);
  const float cz = (value.z < minVal) ? minVal : ((value.z > maxVal) ? maxVal : value.z);
  return Vec3(cx, cy, cz);
}

/// Handles distance.
float distance(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return length(sub(lhs, rhs));
}

} // namespace engine::math
