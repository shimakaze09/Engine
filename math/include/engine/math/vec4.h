// Declares vec4 types and APIs for the Engine math library.

#pragma once

namespace engine::math {

/// Stores alignas data used by the engine.
struct alignas(16) Vec4 final {
  float x;
  float y;
  float z;
  float w;

  /// Handles vec4.
  constexpr Vec4() noexcept : x(0.0F), y(0.0F), z(0.0F), w(0.0F) {}

  /// Handles vec4.
  constexpr Vec4(float xIn, float yIn, float zIn, float wIn) noexcept
      /// Handles x.
      : x(xIn), y(yIn), z(zIn), w(wIn) {}
};

static_assert(alignof(Vec4) == 16U, "Vec4 must stay 16-byte aligned.");
static_assert(sizeof(Vec4) == 16U,
              "Vec4 must stay tightly packed for SIMD handoff.");

/// Adds a value or component to the target system.
Vec4 add(const Vec4 &lhs, const Vec4 &rhs) noexcept;
/// Handles sub.
Vec4 sub(const Vec4 &lhs, const Vec4 &rhs) noexcept;
/// Handles mul.
Vec4 mul(const Vec4 &lhs, float scalar) noexcept;
/// Handles div.
Vec4 div(const Vec4 &lhs, float scalar) noexcept;
/// Handles dot.
float dot(const Vec4 &lhs, const Vec4 &rhs) noexcept;
/// Handles length sq.
float length_sq(const Vec4 &value) noexcept;
/// Handles length.
float length(const Vec4 &value) noexcept;
/// Clamps and fills settings into a safe runtime range.
Vec4 normalize(const Vec4 &value) noexcept;
/// Handles lerp.
Vec4 lerp(const Vec4 &from, const Vec4 &to, float t) noexcept;

} // namespace engine::math
