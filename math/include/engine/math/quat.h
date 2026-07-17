// Rotation quaternion (x, y, z, w) with conversions to/from axis-angle,
// Euler angles, and Mat4, defined inline for hot transform/physics paths.

#pragma once

#include <cmath>

#include "engine/math/mat4.h"
#include "engine/math/math_detail.h"
#include "engine/math/vec3.h"

namespace engine::math {

/// Rotation quaternion; (0, 0, 0, 1) is identity. 16-byte aligned for SIMD.
struct alignas(16) Quat final {
  float x;
  float y;
  float z;
  float w;

  /// Identity rotation.
  constexpr Quat() noexcept : x(0.0F), y(0.0F), z(0.0F), w(1.0F) {}

  /// Component-wise constructor.
  constexpr Quat(float xIn, float yIn, float zIn, float wIn) noexcept
      : x(xIn), y(yIn), z(zIn), w(wIn) {}
};

static_assert(alignof(Quat) == 16U, "Quat must stay 16-byte aligned.");
static_assert(sizeof(Quat) == 16U,
              "Quat must stay tightly packed for SIMD handoff.");

/// Conjugate (inverse rotation for unit quaternions).
inline Quat conjugate(const Quat &value) noexcept {
#if ENGINE_MATH_SSE2
  // Negate x,y,z but keep w: multiply by (-1,-1,-1,+1).
  alignas(16) static const float kSignMask[4] = {-1.0F, -1.0F, -1.0F, 1.0F};
  __m128 q = _mm_load_ps(&value.x);
  __m128 mask = _mm_load_ps(kSignMask);
  Quat result;
  _mm_store_ps(&result.x, _mm_mul_ps(q, mask));
  return result;
#else
  return Quat(-value.x, -value.y, -value.z, value.w);
#endif
}

/// Four-component dot product.
inline float dot(const Quat &lhs, const Quat &rhs) noexcept {
#if ENGINE_MATH_SSE2
  __m128 a = _mm_load_ps(&lhs.x);
  __m128 b = _mm_load_ps(&rhs.x);
  return detail::sse2_hsum(_mm_mul_ps(a, b));
#else
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z) + (lhs.w * rhs.w);
#endif
}

/// Hamilton product: the rotation rhs followed by lhs.
constexpr Quat mul(const Quat &lhs, const Quat &rhs) noexcept {
  return Quat(lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
              lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
              lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
              lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z);
}

/// Unit-length copy; a zero quaternion normalizes to identity.
inline Quat normalize(const Quat &value) noexcept {
  const float lenSq = dot(value, value);
  if (lenSq <= 0.0F) {
    return Quat();
  }

  const float invLen = 1.0F / std::sqrt(lenSq);
#if ENGINE_MATH_SSE2
  __m128 q = _mm_load_ps(&value.x);
  __m128 s = _mm_set1_ps(invLen);
  Quat result;
  _mm_store_ps(&result.x, _mm_mul_ps(q, s));
  return result;
#else
  return Quat(value.x * invLen, value.y * invLen, value.z * invLen,
              value.w * invLen);
#endif
}

/// Spherical interpolation along the shortest arc; falls back to normalized
/// lerp when the quaternions are nearly parallel.
inline Quat slerp(const Quat &from, const Quat &to, float t) noexcept {
  Quat end = to;
  float cosTheta = dot(from, end);

  if (cosTheta < 0.0F) {
    cosTheta = -cosTheta;
    end = Quat(-end.x, -end.y, -end.z, -end.w);
  }

  if (cosTheta > 0.9995F) {
    const Quat lerpResult(
        from.x + (end.x - from.x) * t, from.y + (end.y - from.y) * t,
        from.z + (end.z - from.z) * t, from.w + (end.w - from.w) * t);
    return normalize(lerpResult);
  }

  const float theta = std::acos(detail::clamp_scalar(cosTheta, -1.0F, 1.0F));
  const float sinTheta = std::sin(theta);
  const float invSinTheta = (sinTheta != 0.0F) ? (1.0F / sinTheta) : 0.0F;

  const float scaleFrom = std::sin((1.0F - t) * theta) * invSinTheta;
  const float scaleTo = std::sin(t * theta) * invSinTheta;

  return Quat(from.x * scaleFrom + end.x * scaleTo,
              from.y * scaleFrom + end.y * scaleTo,
              from.z * scaleFrom + end.z * scaleTo,
              from.w * scaleFrom + end.w * scaleTo);
}

/// Rotation of `radians` about `axis`; a near-zero axis yields identity.
inline Quat from_axis_angle(const Vec3 &axis, float radians) noexcept {
  const float axisLengthSq = dot(axis, axis);
  if (axisLengthSq <= 1.0e-12F) {
    return Quat();
  }

  const Vec3 normalizedAxis = normalize(axis);
  const float halfAngle = 0.5F * radians;
  const float sinHalf = std::sin(halfAngle);

  return Quat(normalizedAxis.x * sinHalf, normalizedAxis.y * sinHalf,
              normalizedAxis.z * sinHalf, std::cos(halfAngle));
}

/// Extracts the rotation axis and angle; identity maps to the +X axis.
inline bool to_axis_angle(const Quat &value, Vec3 *outAxis,
                          float *outRadians) noexcept {
  if ((outAxis == nullptr) || (outRadians == nullptr)) {
    return false;
  }

  const Quat normalized = normalize(value);
  const float angle =
      2.0F * std::acos(detail::clamp_scalar(normalized.w, -1.0F, 1.0F));
  const float sinHalf = std::sqrt(1.0F - (normalized.w * normalized.w));

  if (sinHalf <= 1.0e-6F) {
    *outAxis = Vec3(1.0F, 0.0F, 0.0F);
  } else {
    *outAxis = Vec3(normalized.x / sinHalf, normalized.y / sinHalf,
                    normalized.z / sinHalf);
  }

  *outRadians = angle;
  return true;
}

/// Rotation matrix for a (normalized copy of the) quaternion.
inline Mat4 to_mat4(const Quat &value) noexcept {
  const Quat n = normalize(value);
  const float xx = n.x * n.x;
  const float yy = n.y * n.y;
  const float zz = n.z * n.z;
  const float xy = n.x * n.y;
  const float xz = n.x * n.z;
  const float yz = n.y * n.z;
  const float wx = n.w * n.x;
  const float wy = n.w * n.y;
  const float wz = n.w * n.z;

  return Mat4(
      Vec4(1.0F - 2.0F * (yy + zz), 2.0F * (xy + wz), 2.0F * (xz - wy), 0.0F),
      Vec4(2.0F * (xy - wz), 1.0F - 2.0F * (xx + zz), 2.0F * (yz + wx), 0.0F),
      Vec4(2.0F * (xz + wy), 2.0F * (yz - wx), 1.0F - 2.0F * (xx + yy), 0.0F),
      Vec4(0.0F, 0.0F, 0.0F, 1.0F));
}

/// Rotation quaternion from a pure rotation matrix (Shepperd's method).
inline Quat from_mat4(const Mat4 &value) noexcept {
  const float m00 = value.columns[0].x;
  const float m01 = value.columns[1].x;
  const float m02 = value.columns[2].x;
  const float m10 = value.columns[0].y;
  const float m11 = value.columns[1].y;
  const float m12 = value.columns[2].y;
  const float m20 = value.columns[0].z;
  const float m21 = value.columns[1].z;
  const float m22 = value.columns[2].z;

  const float trace = m00 + m11 + m22;
  if (trace > 0.0F) {
    const float s = std::sqrt(trace + 1.0F) * 0.5F;
    const float inv = 0.25F / s;
    return Quat((m21 - m12) * inv, (m02 - m20) * inv, (m10 - m01) * inv, s);
  }

  if ((m00 > m11) && (m00 > m22)) {
    const float s = std::sqrt(1.0F + m00 - m11 - m22) * 0.5F;
    const float inv = 0.25F / s;
    return Quat(s, (m01 + m10) * inv, (m02 + m20) * inv, (m21 - m12) * inv);
  }

  if (m11 > m22) {
    const float s = std::sqrt(1.0F + m11 - m00 - m22) * 0.5F;
    const float inv = 0.25F / s;
    return Quat((m01 + m10) * inv, s, (m12 + m21) * inv, (m02 - m20) * inv);
  }

  const float s = std::sqrt(1.0F + m22 - m00 - m11) * 0.5F;
  const float inv = 0.25F / s;
  return Quat((m02 + m20) * inv, (m12 + m21) * inv, s, (m10 - m01) * inv);
}

/// Rotates a vector by a unit quaternion (Rodrigues form:
/// t = 2*cross(q.xyz, v); result = v + q.w*t + cross(q.xyz, t)).
constexpr Vec3 rotate_vector(const Vec3 &v, const Quat &q) noexcept {
  const Vec3 qxyz(q.x, q.y, q.z);
  const Vec3 t = mul(cross(qxyz, v), 2.0F);
  return add(add(v, mul(t, q.w)), cross(qxyz, t));
}

// Euler convention: pitch is rotation about +X, yaw about +Y, roll about +Z.
// Composition order is q = qy(yaw) * qx(pitch) * qz(roll).
inline Quat from_euler(float pitchRad, float yawRad, float rollRad) noexcept {
  const float cy = std::cos(yawRad * 0.5F);
  const float sy = std::sin(yawRad * 0.5F);
  const float cp = std::cos(pitchRad * 0.5F);
  const float sp = std::sin(pitchRad * 0.5F);
  const float cr = std::cos(rollRad * 0.5F);
  const float sr = std::sin(rollRad * 0.5F);

  return Quat(cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr,
              cy * cp * sr - sy * sp * cr, cy * cp * cr + sy * sp * sr);
}

/// Extracts Euler angles using the same axis convention as from_euler.
inline bool to_euler(const Quat &q, float *outPitch, float *outYaw,
                     float *outRoll) noexcept {
  if ((outPitch == nullptr) || (outYaw == nullptr) || (outRoll == nullptr)) {
    return false;
  }

  const float sinPitchCosp = 2.0F * (q.w * q.x + q.y * q.z);
  const float cosPitchCosp = 1.0F - 2.0F * (q.x * q.x + q.y * q.y);
  *outPitch = std::atan2(sinPitchCosp, cosPitchCosp);

  const float sinYaw = 2.0F * (q.w * q.y - q.z * q.x);
  if (std::fabs(sinYaw) >= 1.0F) {
    *outYaw = std::copysign(1.5707963268F, sinYaw);
  } else {
    *outYaw = std::asin(sinYaw);
  }

  const float sinRollCosp = 2.0F * (q.w * q.z + q.x * q.y);
  const float cosRollCosp = 1.0F - 2.0F * (q.y * q.y + q.z * q.z);
  *outRoll = std::atan2(sinRollCosp, cosRollCosp);

  return true;
}

} // namespace engine::math
