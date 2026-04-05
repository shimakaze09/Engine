#pragma once

namespace engine::math {

struct Vec3 final {
  float x;
  float y;
  float z;

  constexpr Vec3() noexcept : x(0.0F), y(0.0F), z(0.0F) {}

  constexpr Vec3(float xIn, float yIn, float zIn) noexcept
      : x(xIn), y(yIn), z(zIn) {}
};

Vec3 add(const Vec3 &lhs, const Vec3 &rhs) noexcept;
Vec3 sub(const Vec3 &lhs, const Vec3 &rhs) noexcept;
Vec3 mul(const Vec3 &lhs, float scalar) noexcept;
Vec3 div(const Vec3 &lhs, float scalar) noexcept;
float dot(const Vec3 &lhs, const Vec3 &rhs) noexcept;
Vec3 cross(const Vec3 &lhs, const Vec3 &rhs) noexcept;
float length_sq(const Vec3 &value) noexcept;
float length(const Vec3 &value) noexcept;
Vec3 normalize(const Vec3 &value) noexcept;
Vec3 lerp(const Vec3 &from, const Vec3 &to, float t) noexcept;

} // namespace engine::math
