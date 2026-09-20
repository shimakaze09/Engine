// Deterministic scalar transcendentals for the simulation: sin, cos, tan,
// exp, log, atan, atan2, asin and acos evaluated with IEEE add, multiply,
// divide and sqrt only (plus the exact ldexp, frexp, fabs and copysign),
// so every platform and compiler under the engine's strict float flags
// produces the same bits. libm's versions are not correctly rounded and
// differ between glibc, UCRT and libSystem, so the fixed step, the
// quaternion helpers and the Lua math table call these instead. Accuracy
// is a few ulp over the simulation's ranges; arguments beyond about 8192
// radians lose range-reduction precision but stay deterministic.

#pragma once

#include <cmath>
#include <limits>

namespace engine::math {

constexpr float kDetPi = 3.14159265358979323846F;
constexpr float kDetHalfPi = 1.57079632679489661923F;

namespace detail {

// pi/2 split so that k * piece is exact for the integer quadrant counts
// the reduction produces (Cody-Waite; the cephes constants).
constexpr float kPio2Hi = 1.5703125F;
constexpr float kPio2Mid = 4.837512969970703125e-4F;
constexpr float kPio2Lo = 7.549789948768648e-8F;
constexpr float kTwoOverPi = 0.636619772367581343F;
// ln 2 split the same way for exp's reduction.
constexpr float kLn2Hi = 0.693145751953125F;
constexpr float kLn2Lo = 1.428606765330187e-6F;
constexpr float kOneOverLn2 = 1.44269504088896341F;

/// Nearest integer, halves away from zero; exact for |value| < 2^23.
inline int round_to_int(float value) noexcept {
  return static_cast<int>(value + ((value >= 0.0F) ? 0.5F : -0.5F));
}

/// sin on |r| <= pi/4 by its Taylor series to r^11 (truncation below
/// 1e-11 there).
inline float sin_reduced(float r) noexcept {
  const float r2 = r * r;
  float p = -1.0F / 39916800.0F;
  p = (p * r2) + (1.0F / 362880.0F);
  p = (p * r2) - (1.0F / 5040.0F);
  p = (p * r2) + (1.0F / 120.0F);
  p = (p * r2) - (1.0F / 6.0F);
  p = (p * r2) + 1.0F;
  return r * p;
}

/// cos on |r| <= pi/4 by its Taylor series to r^12.
inline float cos_reduced(float r) noexcept {
  const float r2 = r * r;
  float p = 1.0F / 479001600.0F;
  p = (p * r2) - (1.0F / 3628800.0F);
  p = (p * r2) + (1.0F / 40320.0F);
  p = (p * r2) - (1.0F / 720.0F);
  p = (p * r2) + (1.0F / 24.0F);
  p = (p * r2) - 0.5F;
  p = (p * r2) + 1.0F;
  return p;
}

/// Reduces `x` to a quadrant index and a remainder in [-pi/4, pi/4].
inline float reduce_quadrant(float x, int *outQuadrant) noexcept {
  const int k = round_to_int(x * kTwoOverPi);
  const float kf = static_cast<float>(k);
  const float r = ((x - (kf * kPio2Hi)) - (kf * kPio2Mid)) - (kf * kPio2Lo);
  *outQuadrant = ((k % 4) + 4) % 4;
  return r;
}

/// atan on |t| <= tan(pi/16) by its Taylor series to t^13.
inline float atan_reduced(float t) noexcept {
  const float t2 = t * t;
  float p = 1.0F / 13.0F;
  p = (p * t2) - (1.0F / 11.0F);
  p = (p * t2) + (1.0F / 9.0F);
  p = (p * t2) - (1.0F / 7.0F);
  p = (p * t2) + (1.0F / 5.0F);
  p = (p * t2) - (1.0F / 3.0F);
  p = (p * t2) + 1.0F;
  return t * p;
}

} // namespace detail

/// Deterministic sin.
inline float det_sin(float x) noexcept {
  int quadrant = 0;
  const float r = detail::reduce_quadrant(x, &quadrant);
  switch (quadrant) {
  case 0:
    return detail::sin_reduced(r);
  case 1:
    return detail::cos_reduced(r);
  case 2:
    return -detail::sin_reduced(r);
  default:
    return -detail::cos_reduced(r);
  }
}

/// Deterministic cos.
inline float det_cos(float x) noexcept {
  int quadrant = 0;
  const float r = detail::reduce_quadrant(x, &quadrant);
  switch (quadrant) {
  case 0:
    return detail::cos_reduced(r);
  case 1:
    return -detail::sin_reduced(r);
  case 2:
    return -detail::cos_reduced(r);
  default:
    return detail::sin_reduced(r);
  }
}

/// Deterministic tan as the quotient of the deterministic sin and cos;
/// grows without bound toward odd multiples of pi/2 as the quotient does.
inline float det_tan(float x) noexcept { return det_sin(x) / det_cos(x); }

/// Deterministic natural log: NaN for negative or NaN input, -infinity at
/// zero, infinity at infinity. The mantissa is centered on 1 and its log
/// taken as 2 atanh((m - 1) / (m + 1)) by the series to the 15th power,
/// with the exponent's multiple of ln 2 added in two exact pieces.
inline float det_log(float x) noexcept {
  if (std::isnan(x) || (x < 0.0F)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  if (x == 0.0F) {
    return -std::numeric_limits<float>::infinity();
  }
  if (std::isinf(x)) {
    return x;
  }
  int exponent = 0;
  float m = std::frexp(x, &exponent);
  // frexp gives m in [0.5, 1); shifting the lower half up centers the
  // series on 1, so |s| stays below 0.172.
  if (m < 0.70710678118654752F) {
    m *= 2.0F;
    exponent -= 1;
  }
  const float s = (m - 1.0F) / (m + 1.0F);
  const float s2 = s * s;
  float p = 1.0F / 15.0F;
  p = (p * s2) + (1.0F / 13.0F);
  p = (p * s2) + (1.0F / 11.0F);
  p = (p * s2) + (1.0F / 9.0F);
  p = (p * s2) + (1.0F / 7.0F);
  p = (p * s2) + (1.0F / 5.0F);
  p = (p * s2) + (1.0F / 3.0F);
  p = (p * s2) + 1.0F;
  const float e = static_cast<float>(exponent);
  return ((e * detail::kLn2Hi) + (e * detail::kLn2Lo)) + (2.0F * (s * p));
}

/// Deterministic exp: e^x, overflowing to infinity and underflowing to
/// zero at the same magnitudes as float allows.
inline float det_exp(float x) noexcept {
  if (x > 88.72F) {
    return std::numeric_limits<float>::infinity();
  }
  if (x < -103.98F) {
    return 0.0F;
  }
  const int k = detail::round_to_int(x * detail::kOneOverLn2);
  const float kf = static_cast<float>(k);
  const float r = (x - (kf * detail::kLn2Hi)) - (kf * detail::kLn2Lo);
  // e^r on |r| <= ln2 / 2 by the Taylor series to r^9.
  float p = 1.0F / 362880.0F;
  p = (p * r) + (1.0F / 40320.0F);
  p = (p * r) + (1.0F / 5040.0F);
  p = (p * r) + (1.0F / 720.0F);
  p = (p * r) + (1.0F / 120.0F);
  p = (p * r) + (1.0F / 24.0F);
  p = (p * r) + (1.0F / 6.0F);
  p = (p * r) + 0.5F;
  p = (p * r) + 1.0F;
  p = (p * r) + 1.0F;
  return std::ldexp(p, k);
}

/// Deterministic atan, by two half-angle steps and a short series.
inline float det_atan(float x) noexcept {
  const bool negative = x < 0.0F;
  float a = std::fabs(x);
  const bool inverted = a > 1.0F;
  if (inverted) {
    a = 1.0F / a;
  }
  // atan(a) = 2 atan(a / (1 + sqrt(1 + a^2))), applied twice: the series
  // then runs on |t| <= tan(pi/16).
  const float t1 = a / (1.0F + std::sqrt(1.0F + (a * a)));
  const float t2 = t1 / (1.0F + std::sqrt(1.0F + (t1 * t1)));
  float result = 4.0F * detail::atan_reduced(t2);
  if (inverted) {
    result = kDetHalfPi - result;
  }
  return negative ? -result : result;
}

/// Deterministic atan2 with the C library's quadrant and signed-zero
/// conventions on finite input.
inline float det_atan2(float y, float x) noexcept {
  if (x > 0.0F) {
    return det_atan(y / x);
  }
  if (x < 0.0F) {
    return (std::signbit(y)) ? (det_atan(y / x) - kDetPi)
                             : (det_atan(y / x) + kDetPi);
  }
  if (y > 0.0F) {
    return kDetHalfPi;
  }
  if (y < 0.0F) {
    return -kDetHalfPi;
  }
  return std::signbit(x) ? std::copysign(kDetPi, y) : std::copysign(0.0F, y);
}

/// Deterministic asin on [-1, 1]; input beyond it is clamped.
inline float det_asin(float x) noexcept {
  const float c = (x > 1.0F) ? 1.0F : ((x < -1.0F) ? -1.0F : x);
  return det_atan2(c, std::sqrt((1.0F - c) * (1.0F + c)));
}

/// Deterministic acos on [-1, 1]; input beyond it is clamped.
inline float det_acos(float x) noexcept {
  const float c = (x > 1.0F) ? 1.0F : ((x < -1.0F) ? -1.0F : x);
  if (c <= -1.0F) {
    return kDetPi;
  }
  return 2.0F * det_atan(std::sqrt((1.0F - c) / (1.0F + c)));
}

} // namespace engine::math
