// 3D float vector and its arithmetic, defined inline so hot callers (physics
// contacts, transform propagation, render prep) pay no cross-module call cost.

#pragma once

#include <cmath>

namespace engine::math {

/// 3D float vector (x, y, z).
struct Vec3 final {
  float x;
  float y;
  float z;

  /// Zero vector.
  constexpr Vec3() noexcept : x(0.0F), y(0.0F), z(0.0F) {}

  /// Component-wise constructor.
  constexpr Vec3(float xIn, float yIn, float zIn) noexcept
      : x(xIn), y(yIn), z(zIn) {}
};

/// Component-wise sum.
constexpr Vec3 add(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return Vec3(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z);
}

/// Component-wise difference.
constexpr Vec3 sub(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return Vec3(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z);
}

/// Uniform scale.
constexpr Vec3 mul(const Vec3 &lhs, float scalar) noexcept {
  return Vec3(lhs.x * scalar, lhs.y * scalar, lhs.z * scalar);
}

/// Uniform inverse scale; division by zero yields the zero vector.
constexpr Vec3 div(const Vec3 &lhs, float scalar) noexcept {
  const float inv = (scalar != 0.0F) ? (1.0F / scalar) : 0.0F;
  return Vec3(lhs.x * inv, lhs.y * inv, lhs.z * inv);
}

/// Dot product.
constexpr float dot(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

/// Right-handed cross product.
constexpr Vec3 cross(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return Vec3((lhs.y * rhs.z) - (lhs.z * rhs.y),
              (lhs.z * rhs.x) - (lhs.x * rhs.z),
              (lhs.x * rhs.y) - (lhs.y * rhs.x));
}

/// Squared length (avoids the sqrt when only comparing magnitudes).
constexpr float length_sq(const Vec3 &value) noexcept {
  return dot(value, value);
}

/// Euclidean length.
inline float length(const Vec3 &value) noexcept {
  return std::sqrt(length_sq(value));
}

/// Unit-length copy; the zero vector normalizes to zero.
inline Vec3 normalize(const Vec3 &value) noexcept {
  const float len = length(value);
  if (len <= 0.0F) {
    return Vec3();
  }

  return div(value, len);
}

/// Linear interpolation: from + (to - from) * t.
constexpr Vec3 lerp(const Vec3 &from, const Vec3 &to, float t) noexcept {
  return add(from, mul(sub(to, from), t));
}

/// Component-wise negation.
constexpr Vec3 negate(const Vec3 &value) noexcept {
  return Vec3(-value.x, -value.y, -value.z);
}

/// Mirror of an incident vector about a unit normal.
constexpr Vec3 reflect(const Vec3 &incident, const Vec3 &normal) noexcept {
  const float d = 2.0F * dot(incident, normal);
  return sub(incident, mul(normal, d));
}

/// Component-wise clamp to [minVal, maxVal].
constexpr Vec3 clamp(const Vec3 &value, float minVal, float maxVal) noexcept {
  const float cx =
      (value.x < minVal) ? minVal : ((value.x > maxVal) ? maxVal : value.x);
  const float cy =
      (value.y < minVal) ? minVal : ((value.y > maxVal) ? maxVal : value.y);
  const float cz =
      (value.z < minVal) ? minVal : ((value.z > maxVal) ? maxVal : value.z);
  return Vec3(cx, cy, cz);
}

/// Euclidean distance between two points.
inline float distance(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return length(sub(lhs, rhs));
}

} // namespace engine::math
