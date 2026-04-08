#pragma once

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"

namespace engine::math {

struct alignas(16) Quat final {
  float x;
  float y;
  float z;
  float w;

  constexpr Quat() noexcept : x(0.0F), y(0.0F), z(0.0F), w(1.0F) {}

  constexpr Quat(float xIn, float yIn, float zIn, float wIn) noexcept
      : x(xIn), y(yIn), z(zIn), w(wIn) {}
};

static_assert(alignof(Quat) == 16U, "Quat must stay 16-byte aligned.");
static_assert(sizeof(Quat) == 16U,
              "Quat must stay tightly packed for SIMD handoff.");

Quat conjugate(const Quat &value) noexcept;
float dot(const Quat &lhs, const Quat &rhs) noexcept;
Quat mul(const Quat &lhs, const Quat &rhs) noexcept;
Quat normalize(const Quat &value) noexcept;
Quat slerp(const Quat &from, const Quat &to, float t) noexcept;
Quat from_axis_angle(const Vec3 &axis, float radians) noexcept;
bool to_axis_angle(const Quat &value, Vec3 *outAxis,
                   float *outRadians) noexcept;
Mat4 to_mat4(const Quat &value) noexcept;
Quat from_mat4(const Mat4 &value) noexcept;
Vec3 rotate_vector(const Vec3 &v, const Quat &q) noexcept;
// Euler convention: pitch is rotation about +X, yaw about +Y, roll about +Z.
// Composition order is q = qy(yaw) * qx(pitch) * qz(roll).
Quat from_euler(float pitchRad, float yawRad, float rollRad) noexcept;
bool to_euler(const Quat &q, float *outPitch, float *outYaw,
              float *outRoll) noexcept;

} // namespace engine::math
