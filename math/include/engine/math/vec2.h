// Declares vec2 types and APIs for the Engine math library.

#pragma once

namespace engine::math {

/// Stores vec2 data used by the engine.
struct Vec2 final {
  float x;
  float y;

  /// Handles vec2.
  constexpr Vec2() noexcept : x(0.0F), y(0.0F) {}

  /// Handles vec2.
  constexpr Vec2(float xIn, float yIn) noexcept : x(xIn), y(yIn) {}
};

/// Adds a value or component to the target system.
Vec2 add(const Vec2 &lhs, const Vec2 &rhs) noexcept;
/// Handles sub.
Vec2 sub(const Vec2 &lhs, const Vec2 &rhs) noexcept;
/// Handles mul.
Vec2 mul(const Vec2 &lhs, float scalar) noexcept;
/// Handles div.
Vec2 div(const Vec2 &lhs, float scalar) noexcept;
/// Handles dot.
float dot(const Vec2 &lhs, const Vec2 &rhs) noexcept;
/// Handles length sq.
float length_sq(const Vec2 &value) noexcept;
/// Handles length.
float length(const Vec2 &value) noexcept;
/// Clamps and fills settings into a safe runtime range.
Vec2 normalize(const Vec2 &value) noexcept;
/// Handles lerp.
Vec2 lerp(const Vec2 &from, const Vec2 &to, float t) noexcept;

} // namespace engine::math
