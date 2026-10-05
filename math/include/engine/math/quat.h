// Rotation quaternion (x, y, z, w) with conversions to/from axis-angle,
// Euler angles, Mat4 and a look direction, defined inline for hot
// transform/physics paths. Every trigonometric evaluation goes through the
// deterministic scalar set, so a rotation integrated on one platform matches
// another bit for bit.

#pragma once

#include <cmath>

#include "engine/math/mat4.h"
#include "engine/math/math_detail.h"
#include "engine/math/scalar.h"
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
  // Pairwise, matching sse2_hsum's (xx + yy) + (zz + ww), so scalar and
  // SSE2 builds round identically; engine_unit_math_parity pins this.
  return ((lhs.x * rhs.x) + (lhs.y * rhs.y)) +
         ((lhs.z * rhs.z) + (lhs.w * rhs.w));
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

  const float theta = det_acos(detail::clamp_scalar(cosTheta, -1.0F, 1.0F));
  const float sinTheta = det_sin(theta);
  const float invSinTheta = (sinTheta != 0.0F) ? (1.0F / sinTheta) : 0.0F;

  const float scaleFrom = det_sin((1.0F - t) * theta) * invSinTheta;
  const float scaleTo = det_sin(t * theta) * invSinTheta;

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
  const float sinHalf = det_sin(halfAngle);

  return Quat(normalizedAxis.x * sinHalf, normalizedAxis.y * sinHalf,
              normalizedAxis.z * sinHalf, det_cos(halfAngle));
}

/// Extracts the rotation axis and angle; identity maps to the +X axis.
inline bool to_axis_angle(const Quat &value, Vec3 *outAxis,
                          float *outRadians) noexcept {
  if ((outAxis == nullptr) || (outRadians == nullptr)) {
    return false;
  }

  const Quat normalized = normalize(value);
  // Clamped once and used for both derivations: a w that rounded past ±1
  // would otherwise reach acos clamped but sqrt(1 - w²) unclamped, and a
  // negative radicand yields a NaN axis. Not reproduced through normalize()
  // on x86-64 (402,560 near-unit inputs at -O0 and -O2), so this is
  // defence-in-depth for other toolchains, not a fix to observed output.
  const float w = detail::clamp_scalar(normalized.w, -1.0F, 1.0F);
  const float angle = 2.0F * det_acos(w);
  const float sinHalf = std::sqrt(1.0F - (w * w));

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

/// The rotation that points an entity's forward (-Z, the camera
/// convention) along `forward`, with its +Y as near `up` as the forward
/// allows. False, `*out` untouched, when `forward` is zero or parallel to
/// `up`, where no one rotation is meant; neither needs to be unit length.
inline bool look_rotation(const Vec3 &forward, const Vec3 &up,
                          Quat *out) noexcept {
  const Vec3 back = normalize(Vec3(-forward.x, -forward.y, -forward.z));
  const Vec3 right = cross(up, back);
  const float rightLength = length(right);
  if ((out == nullptr) || (length(back) <= 0.0F) ||
      !(rightLength > 1.0e-6F * length(up))) {
    return false;
  }
  const Vec3 x = mul(right, 1.0F / rightLength);
  const Vec3 y = cross(back, x);
  *out = normalize(from_mat4(
      Mat4(Vec4(x.x, x.y, x.z, 0.0F), Vec4(y.x, y.y, y.z, 0.0F),
           Vec4(back.x, back.y, back.z, 0.0F), Vec4(0.0F, 0.0F, 0.0F, 1.0F))));
  return true;
}

/// The inverse rotation of any non-zero quaternion: its conjugate over its
/// squared length. For a unit quaternion this is the conjugate; a zero
/// quaternion gives identity.
inline Quat inverse(const Quat &value) noexcept {
  const float lengthSq = dot(value, value);
  if (!(lengthSq > 0.0F)) {
    return Quat();
  }
  const float inv = 1.0F / lengthSq;
  return Quat(-value.x * inv, -value.y * inv, -value.z * inv, value.w * inv);
}

/// Rotates `v` by the inverse of the unit quaternion `q`: a world-space
/// vector into the frame `q` orients.
constexpr Vec3 rotate_inverse(const Vec3 &v, const Quat &q) noexcept {
  return rotate_vector(v, Quat(-q.x, -q.y, -q.z, q.w));
}

/// The rotation from unit `from` to unit `to` expressed in `from`'s frame,
/// from^-1 * to: what `to` is relative to `from`.
inline Quat relative(const Quat &from, const Quat &to) noexcept {
  return mul(conjugate(from), to);
}

/// Normalized lerp along the shortest arc: cheaper than slerp, with the
/// same endpoints and path but not constant angular speed. Not clamped.
inline Quat nlerp(const Quat &from, Quat to, float t) noexcept {
  if (dot(from, to) < 0.0F) {
    to = Quat(-to.x, -to.y, -to.z, -to.w);
  }
  return normalize(Quat(from.x + ((to.x - from.x) * t),
                        from.y + ((to.y - from.y) * t),
                        from.z + ((to.z - from.z) * t),
                        from.w + ((to.w - from.w) * t)));
}

/// The direction an entity rotated by unit `q` faces: its -Z (the camera
/// convention look_rotation uses).
constexpr Vec3 forward(const Quat &q) noexcept {
  return rotate_vector(Vec3(0.0F, 0.0F, -1.0F), q);
}

/// The entity's +X.
constexpr Vec3 right(const Quat &q) noexcept {
  return rotate_vector(Vec3(1.0F, 0.0F, 0.0F), q);
}

/// The entity's +Y.
constexpr Vec3 up(const Quat &q) noexcept {
  return rotate_vector(Vec3(0.0F, 1.0F, 0.0F), q);
}

/// The entity's +Z, the opposite of forward.
constexpr Vec3 back(const Quat &q) noexcept {
  return rotate_vector(Vec3(0.0F, 0.0F, 1.0F), q);
}

/// The angle between two unit rotations, in [0, pi] radians.
inline float angle(const Quat &a, const Quat &b) noexcept {
  const float d = std::fabs(dot(a, b));
  return 2.0F * det_acos((d < 1.0F) ? d : 1.0F);
}

/// The shortest rotation turning direction `from` onto direction `to`;
/// neither needs to be unit length. Identity when either is zero. Opposite
/// directions turn half a turn about an axis perpendicular to `from`.
inline Quat from_to(const Vec3 &from, const Vec3 &to) noexcept {
  const float fromLength = length(from);
  const float toLength = length(to);
  if (!(fromLength > 0.0F) || !(toLength > 0.0F)) {
    return Quat();
  }
  const Vec3 f = mul(from, 1.0F / fromLength);
  const Vec3 t = mul(to, 1.0F / toLength);
  const float d = dot(f, t);
  if (d <= -0.999999F) {
    // Any axis perpendicular to f: cross with the basis vector least
    // aligned with it.
    const Vec3 basis = (std::fabs(f.x) < 0.57735F) ? Vec3(1.0F, 0.0F, 0.0F)
                                                   : Vec3(0.0F, 1.0F, 0.0F);
    const Vec3 axis = normalize(cross(f, basis));
    return Quat(axis.x, axis.y, axis.z, 0.0F);
  }
  const Vec3 c = cross(f, t);
  return normalize(Quat(c.x, c.y, c.z, 1.0F + d));
}

/// Turns unit `from` towards unit `to` by at most `maxRadians`, landing on
/// `to` when it is within reach (Unity's RotateTowards).
inline Quat rotate_towards(const Quat &from, const Quat &to,
                           float maxRadians) noexcept {
  const float total = angle(from, to);
  if ((total == 0.0F) || (total <= maxRadians)) {
    return to;
  }
  return slerp(from, to, maxRadians / total);
}

/// Unit `q` after turning at angular velocity `omega` (radians per second,
/// world axes) for `dt` seconds, renormalized; unchanged when the speed is
/// below 1e-6 rad/s. The physics step's integration, moved here verbatim so
/// its operations, and the simulation's bits, are unchanged.
inline Quat integrate(const Quat &q, const Vec3 &omega, float dt) noexcept {
  const float speedSq = length_sq(omega);
  if (!(speedSq > 1e-12F)) {
    return q;
  }
  const float speed = std::sqrt(speedSq);
  const Quat delta = from_axis_angle(div(omega, speed), speed * dt);
  return normalize(mul(delta, q));
}

/// The rotation vector (axis times angle, radians) of `q` along its
/// shorter arc, so its length is at most pi; zero for identity.
inline Vec3 to_rotation_vector(Quat q) noexcept {
  if (q.w < 0.0F) {
    q = Quat(-q.x, -q.y, -q.z, -q.w);
  }
  Vec3 axis{};
  float radians = 0.0F;
  if (!to_axis_angle(q, &axis, &radians)) {
    return Vec3(0.0F, 0.0F, 0.0F);
  }
  return mul(axis, radians);
}

/// The rotation a rotation vector (axis times angle) describes; identity
/// below 1e-6 radians.
inline Quat from_rotation_vector(const Vec3 &rotation) noexcept {
  const float radiansSq = length_sq(rotation);
  if (radiansSq <= 1.0e-6F * 1.0e-6F) {
    return Quat();
  }
  const float radians = std::sqrt(radiansSq);
  return from_axis_angle(div(rotation, radians), radians);
}

/// Splits unit `q` into a twist about unit `axis` and the swing that
/// follows it, q = swing * twist, as Jolt's GetSwingTwist does. When q
/// turns the axis by exactly half a turn the twist is undefined and is
/// reported as identity, so swing carries all of q.
inline void swing_twist(const Quat &q, const Vec3 &axis, Quat *outSwing,
                        Quat *outTwist) noexcept {
  const float along = (q.x * axis.x) + (q.y * axis.y) + (q.z * axis.z);
  Quat twist(axis.x * along, axis.y * along, axis.z * along, q.w);
  const float twistSq = dot(twist, twist);
  twist = (twistSq > 1.0e-12F) ? normalize(twist) : Quat();
  if (outTwist != nullptr) {
    *outTwist = twist;
  }
  if (outSwing != nullptr) {
    *outSwing = mul(q, conjugate(twist));
  }
}

/// The order Euler angles compose in, named by the axes left to right in
/// the product: YXZ is q = qy(yaw) * qx(pitch) * qz(roll), the engine's
/// default and the editor's. Pitch is always about +X, yaw about +Y and
/// roll about +Z; the order decides only how they compose, which is what
/// differs between tools (Maya and Blender default to XYZ).
enum class EulerOrder : unsigned char { XYZ, XZY, YXZ, YZX, ZXY, ZYX };

namespace detail {

/// The axes of `order`, left to right in the product, as 0 = X, 1 = Y,
/// 2 = Z.
constexpr void euler_axes(EulerOrder order, int *a, int *b, int *c) noexcept {
  constexpr int kAxes[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                               {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  const int index = static_cast<int>(order);
  *a = kAxes[index][0];
  *b = kAxes[index][1];
  *c = kAxes[index][2];
}

/// Rotation of `radians` about basis axis `axis` (0 = X, 1 = Y, 2 = Z).
inline Quat axis_rotation(int axis, float radians) noexcept {
  const float s = det_sin(radians * 0.5F);
  const float c = det_cos(radians * 0.5F);
  if (axis == 0) {
    return Quat(s, 0.0F, 0.0F, c);
  }
  if (axis == 1) {
    return Quat(0.0F, s, 0.0F, c);
  }
  return Quat(0.0F, 0.0F, s, c);
}

/// The factors of the off-diagonal rotation matrix element (row, col) of
/// `q`: 2 * ((lo * hi) + (w * third)) when `plus`, else with a minus.
struct RotationTerms final {
  float lo;
  float hi;
  float third;
  bool plus;
};

inline RotationTerms rotation_terms(const Quat &q, int row,
                                    int col) noexcept {
  const int first = (row < col) ? row : col;
  const int second = (row < col) ? col : row;
  const int other = 3 - row - col;
  RotationTerms terms{};
  terms.lo = (first == 0) ? q.x : q.y;
  terms.hi = (second == 2) ? q.z : q.y;
  terms.third = (other == 0) ? q.x : ((other == 1) ? q.y : q.z);
  // The w term is added in the cyclic positions (1, 0), (2, 1) and (0, 2).
  terms.plus = ((col + 1) % 3) == row;
  return terms;
}

/// Element (row, col) of the rotation matrix of `q`, written as the
/// default order's extraction always computed it, so YXZ keeps its bits.
inline float rotation_element(const Quat &q, int row, int col) noexcept {
  if (row == col) {
    const float a = (row == 0) ? q.y : q.x;
    const float b = (row == 2) ? q.y : q.z;
    return 1.0F - (2.0F * ((a * a) + (b * b)));
  }
  const RotationTerms t = rotation_terms(q, row, col);
  return t.plus ? (2.0F * ((t.lo * t.hi) + (q.w * t.third)))
                : (2.0F * ((t.lo * t.hi) - (q.w * t.third)));
}

/// The negated off-diagonal element. A minus-type element is negated by
/// swapping its operands, 2 * ((w * third) - (lo * hi)), which is how the
/// default order's pitch was always computed and, unlike negating the
/// result, gives +0 rather than -0 for a zero element.
inline float negated_rotation_element(const Quat &q, int row,
                                      int col) noexcept {
  const RotationTerms t = rotation_terms(q, row, col);
  return t.plus ? -(2.0F * ((t.lo * t.hi) + (q.w * t.third)))
                : (2.0F * ((q.w * t.third) - (t.lo * t.hi)));
}

} // namespace detail

// Euler convention: pitch is rotation about +X, yaw about +Y, roll about +Z.
// Composition order is q = qy(yaw) * qx(pitch) * qz(roll).
inline Quat from_euler(float pitchRad, float yawRad, float rollRad) noexcept {
  const float cy = det_cos(yawRad * 0.5F);
  const float sy = det_sin(yawRad * 0.5F);
  const float cp = det_cos(pitchRad * 0.5F);
  const float sp = det_sin(pitchRad * 0.5F);
  const float cr = det_cos(rollRad * 0.5F);
  const float sr = det_sin(rollRad * 0.5F);

  return Quat(cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr,
              cy * cp * sr - sy * sp * cr, cy * cp * cr + sy * sp * sr);
}

/// from_euler in any order. YXZ is the closed form above, whose bits the
/// navigation agents and saved scenes already depend on; the other orders
/// compose their three axis rotations.
inline Quat from_euler(float pitchRad, float yawRad, float rollRad,
                       EulerOrder order) noexcept {
  if (order == EulerOrder::YXZ) {
    return from_euler(pitchRad, yawRad, rollRad);
  }
  const float angles[3] = {pitchRad, yawRad, rollRad};
  int a = 0;
  int b = 0;
  int c = 0;
  detail::euler_axes(order, &a, &b, &c);
  return mul(mul(detail::axis_rotation(a, angles[a]),
                 detail::axis_rotation(b, angles[b])),
             detail::axis_rotation(c, angles[c]));
}

/// Extracts Euler angles that reconstruct `q` under from_euler(order).
/// The middle axis of the order is the one that loses a degree of freedom
/// at its poles, so its angle is clamped to +-90 deg (asin domain) while
/// the outer two keep the full +-180 deg range (atan2). At the pole
/// (gimbal lock) the outer two are no longer independent -- only their sum
/// or difference is observable -- so the extraction folds that combined
/// value entirely into the first axis and reports the last as 0, the
/// common engine convention. That still reconstructs the identical
/// rotation, just not necessarily the split that produced q.
inline bool to_euler(const Quat &q, EulerOrder order, float *outPitch,
                     float *outYaw, float *outRoll) noexcept {
  if ((outPitch == nullptr) || (outYaw == nullptr) || (outRoll == nullptr)) {
    return false;
  }
  int a = 0;
  int b = 0;
  int c = 0;
  detail::euler_axes(order, &a, &b, &c);
  // +1 when (a, b, c) is a cyclic permutation of (X, Y, Z).
  const bool even = ((a + 1) % 3) == b;

  // For q = Ra Rb Rc, the matrix element (a, c) is +-sin of the middle
  // angle, with the permutation's parity as its sign.
  float sinMiddle = even ? detail::rotation_element(q, a, c)
                         : detail::negated_rotation_element(q, a, c);
  if (sinMiddle > 1.0F) {
    sinMiddle = 1.0F;
  } else if (sinMiddle < -1.0F) {
    sinMiddle = -1.0F;
  }
  float angles[3] = {0.0F, 0.0F, 0.0F};
  angles[b] = det_asin(sinMiddle);

  if (std::fabs(sinMiddle) > 0.999999F) {
    // Gimbal lock: with the last angle 0, column b of the matrix is the
    // first axis's rotation of the middle axis, whose (b, a) and (b, b)
    // elements stay well-conditioned at the pole and give the combined
    // angle.
    const float sign = (sinMiddle >= 0.0F) ? 1.0F : -1.0F;
    angles[a] = sign * det_atan2(detail::rotation_element(q, b, a),
                                 detail::rotation_element(q, b, b));
    angles[c] = 0.0F;
  } else {
    const float bc = even ? detail::negated_rotation_element(q, b, c)
                          : detail::rotation_element(q, b, c);
    const float ab = even ? detail::negated_rotation_element(q, a, b)
                          : detail::rotation_element(q, a, b);
    angles[a] = det_atan2(bc, detail::rotation_element(q, c, c));
    angles[c] = det_atan2(ab, detail::rotation_element(q, a, a));
  }

  *outPitch = angles[0];
  *outYaw = angles[1];
  *outRoll = angles[2];
  return true;
}

/// to_euler in the default order (YXZ): q = qy(yaw) * qx(pitch) *
/// qz(roll), the Ry*Rx*Rz matrix product (not the aircraft-attitude ZYX
/// formula, which does not round-trip with this from_euler); pitch is the
/// middle angle. Round trips are verified in tests/unit/math_test.cpp.
inline bool to_euler(const Quat &q, float *outPitch, float *outYaw,
                     float *outRoll) noexcept {
  return to_euler(q, EulerOrder::YXZ, outPitch, outYaw, outRoll);
}

} // namespace engine::math
