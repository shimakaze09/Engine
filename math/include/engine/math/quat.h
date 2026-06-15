// Declares quat types and APIs for the Engine math library.

#pragma once

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"

namespace engine::math {

/// Stores alignas data used by the engine.
struct alignas(16) Quat final {
  float x;
  float y;
  float z;
  float w;

  /// Handles quat.
  constexpr Quat() noexcept : x(0.0F), y(0.0F), z(0.0F), w(1.0F) {}

  /// Handles quat.
  constexpr Quat(float xIn, float yIn, float zIn, float wIn) noexcept
      /// Handles x.
      : x(xIn), y(yIn), z(zIn), w(wIn) {}
};

static_assert(alignof(Quat) == 16U, "Quat must stay 16-byte aligned.");
static_assert(sizeof(Quat) == 16U,
              "Quat must stay tightly packed for SIMD handoff.");

/// Handles conjugate.
Quat conjugate(const Quat &value) noexcept;
/// Handles dot.
float dot(const Quat &lhs, const Quat &rhs) noexcept;
/// Handles mul.
Quat mul(const Quat &lhs, const Quat &rhs) noexcept;
/// Clamps and fills settings into a safe runtime range.
Quat normalize(const Quat &value) noexcept;
/// Handles slerp.
Quat slerp(const Quat &from, const Quat &to, float t) noexcept;
/// Handles from axis angle.
Quat from_axis_angle(const Vec3 &axis, float radians) noexcept;
/// Converts axis angle into the target representation.
bool to_axis_angle(const Quat &value, Vec3 *outAxis,
                   float *outRadians) noexcept;
/// Converts mat4 into the target representation.
Mat4 to_mat4(const Quat &value) noexcept;
/// Handles from mat4.
Quat from_mat4(const Mat4 &value) noexcept;
/// Handles rotate vector.
Vec3 rotate_vector(const Vec3 &v, const Quat &q) noexcept;
// Euler convention: pitch is rotation about +X, yaw about +Y, roll about +Z.
// Composition order is q = qy(yaw) * qx(pitch) * qz(roll).
Quat from_euler(float pitchRad, float yawRad, float rollRad) noexcept;
/// Converts euler into the target representation.
bool to_euler(const Quat &q, float *outPitch, float *outYaw,
              float *outRoll) noexcept;

} // namespace engine::math
