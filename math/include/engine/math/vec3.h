// Declares vec3 types and APIs for the Engine math library.

#pragma once

namespace engine::math {

/// Stores vec3 data used by the engine.
struct Vec3 final {
  float x;
  float y;
  float z;

  /// Handles vec3.
  constexpr Vec3() noexcept : x(0.0F), y(0.0F), z(0.0F) {}

  /// Handles vec3.
  constexpr Vec3(float xIn, float yIn, float zIn) noexcept
      /// Handles x.
      : x(xIn), y(yIn), z(zIn) {}
};

/// Adds a value or component to the target system.
Vec3 add(const Vec3 &lhs, const Vec3 &rhs) noexcept;
/// Handles sub.
Vec3 sub(const Vec3 &lhs, const Vec3 &rhs) noexcept;
/// Handles mul.
Vec3 mul(const Vec3 &lhs, float scalar) noexcept;
/// Handles div.
Vec3 div(const Vec3 &lhs, float scalar) noexcept;
/// Handles dot.
float dot(const Vec3 &lhs, const Vec3 &rhs) noexcept;
/// Handles cross.
Vec3 cross(const Vec3 &lhs, const Vec3 &rhs) noexcept;
/// Handles length sq.
float length_sq(const Vec3 &value) noexcept;
/// Handles length.
float length(const Vec3 &value) noexcept;
/// Clamps and fills settings into a safe runtime range.
Vec3 normalize(const Vec3 &value) noexcept;
/// Handles lerp.
Vec3 lerp(const Vec3 &from, const Vec3 &to, float t) noexcept;
/// Handles negate.
Vec3 negate(const Vec3 &value) noexcept;
/// Handles reflect.
Vec3 reflect(const Vec3 &incident, const Vec3 &normal) noexcept;
/// Handles clamp.
Vec3 clamp(const Vec3 &value, float minVal, float maxVal) noexcept;
/// Handles distance.
float distance(const Vec3 &lhs, const Vec3 &rhs) noexcept;

} // namespace engine::math
