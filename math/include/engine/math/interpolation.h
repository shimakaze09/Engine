// Interpolation and smoothing laws: scalar lerp and its inverse, remapping,
// smoothstep, moving towards a target at a bounded rate, the critically
// damped spring (smooth damp), exponential decay that converges at the
// same rate whatever the step size, and angle wrapping. One owner, so a
// camera, a UI fade and a script turning towards a target follow the same
// law instead of each inventing one. Every function is deterministic: the
// only transcendental is det_exp, and floor, fmod and sqrt are exact.

#pragma once

#include <cmath>
#include <limits>

#include "engine/math/scalar.h"
#include "engine/math/vec3.h"

namespace engine::math {

constexpr float kDetTwoPi = 2.0F * kDetPi;

/// a + (b - a) * t. t is not clamped, so t outside [0, 1] extrapolates.
constexpr float lerp(float a, float b, float t) noexcept {
  return a + ((b - a) * t);
}

/// The t for which lerp(a, b, t) == value; 0 when a == b, where every t
/// gives the same value. Not clamped.
constexpr float inverse_lerp(float a, float b, float value) noexcept {
  return (a == b) ? 0.0F : ((value - a) / (b - a));
}

/// Maps `value` from [inA, inB] onto [outA, outB], linearly and without
/// clamping.
constexpr float remap(float value, float inA, float inB, float outA,
                      float outB) noexcept {
  return lerp(outA, outB, inverse_lerp(inA, inB, value));
}

/// Clamps to [0, 1]; NaN gives 0.
constexpr float saturate(float value) noexcept {
  return (value > 0.0F) ? ((value < 1.0F) ? value : 1.0F) : 0.0F;
}

/// Hermite step: 0 at or below edge0, 1 at or above edge1, and
/// t * t * (3 - 2t) of the position between them (GLSL's and Godot's
/// argument order). With edge0 == edge1 it is a hard step at the edge.
constexpr float smoothstep(float edge0, float edge1, float x) noexcept {
  if (edge0 == edge1) {
    return (x < edge0) ? 0.0F : 1.0F;
  }
  const float t = saturate((x - edge0) / (edge1 - edge0));
  return t * t * (3.0F - (2.0F * t));
}

/// Moves `current` towards `target` by at most `maxDelta`, landing on it
/// exactly; a negative maxDelta moves away, as Unity's MoveTowards does.
inline float move_towards(float current, float target,
                          float maxDelta) noexcept {
  const float delta = target - current;
  if (std::fabs(delta) <= maxDelta) {
    return target;
  }
  return current + ((delta > 0.0F) ? maxDelta : -maxDelta);
}

/// Moves the point `current` towards `target` by at most `maxDistance`
/// along the line between them, landing on it exactly.
inline Vec3 move_towards(const Vec3 &current, const Vec3 &target,
                         float maxDistance) noexcept {
  const Vec3 delta = sub(target, current);
  const float distanceSq = length_sq(delta);
  if ((distanceSq == 0.0F) ||
      ((maxDistance >= 0.0F) && (distanceSq <= maxDistance * maxDistance))) {
    return target;
  }
  return add(current, mul(delta, maxDistance / std::sqrt(distanceSq)));
}

/// The fraction of the remaining distance an exponential approach at
/// `rate` per second covers in `dt` seconds: 1 - e^(-rate * dt). Two steps
/// of dt cover what one step of 2 dt does, up to rounding, so a law built
/// on it converges at the same rate whatever the step size. 0 when rate or
/// dt is not positive.
inline float exp_decay_factor(float rate, float dt) noexcept {
  if (!(rate > 0.0F) || !(dt > 0.0F)) {
    return 0.0F;
  }
  return 1.0F - det_exp(-(rate * dt));
}

/// Moves `current` towards `target` by exp_decay_factor(rate, dt) of the
/// remaining distance; the step-size-independent replacement for
/// lerp(current, target, rate * dt).
inline float exp_decay(float current, float target, float rate,
                       float dt) noexcept {
  return lerp(current, target, exp_decay_factor(rate, dt));
}

/// exp_decay for a point.
inline Vec3 exp_decay(const Vec3 &current, const Vec3 &target, float rate,
                      float dt) noexcept {
  return lerp(current, target, exp_decay_factor(rate, dt));
}

namespace detail {

/// The shortest smooth time the spring accepts, as Unity's SmoothDamp
/// clamps it, so omega = 2 / smoothTime stays finite.
constexpr float kMinSmoothTime = 0.0001F;

/// e^(-x) by the rational approximation Game Programming Gems 4 (1.10)
/// gives for the critically damped spring; accurate to under 0.1% for the
/// x a frame produces and free of transcendentals.
constexpr float spring_decay(float x) noexcept {
  return 1.0F / (1.0F + x + (0.48F * x * x) + (0.235F * x * x * x));
}

} // namespace detail

/// Critically damped spring towards `target`, Unity's SmoothDamp: reaches
/// the target in about `smoothTime` seconds without overshooting it,
/// carrying its speed in `*velocity` between calls. `maxSpeed` caps the
/// speed (infinity for none). Returns `current` with `*velocity` unchanged
/// when dt is not positive or `velocity` is null.
inline float smooth_damp(float current, float target, float *velocity,
                         float smoothTime, float maxSpeed,
                         float dt) noexcept {
  if ((velocity == nullptr) || !(dt > 0.0F)) {
    return current;
  }
  const float time =
      (smoothTime > detail::kMinSmoothTime) ? smoothTime
                                            : detail::kMinSmoothTime;
  const float omega = 2.0F / time;
  const float decay = detail::spring_decay(omega * dt);
  const float maxChange = maxSpeed * time;
  float change = current - target;
  if (change > maxChange) {
    change = maxChange;
  } else if (change < -maxChange) {
    change = -maxChange;
  }
  const float goal = current - change;
  const float temp = (*velocity + (omega * change)) * dt;
  *velocity = (*velocity - (omega * temp)) * decay;
  float output = goal + ((change + temp) * decay);
  // Never pass the target: a step that would is clamped onto it.
  if (((target - current) > 0.0F) == (output > target)) {
    output = target;
    *velocity = 0.0F;
  }
  return output;
}

/// smooth_damp for a point; `maxSpeed` caps the speed's magnitude.
inline Vec3 smooth_damp(const Vec3 &current, const Vec3 &target,
                        Vec3 *velocity, float smoothTime, float maxSpeed,
                        float dt) noexcept {
  if ((velocity == nullptr) || !(dt > 0.0F)) {
    return current;
  }
  const float time =
      (smoothTime > detail::kMinSmoothTime) ? smoothTime
                                            : detail::kMinSmoothTime;
  const float omega = 2.0F / time;
  const float decay = detail::spring_decay(omega * dt);
  const float maxChange = maxSpeed * time;
  Vec3 change = sub(current, target);
  const float changeSq = length_sq(change);
  if (changeSq > maxChange * maxChange) {
    change = mul(change, maxChange / std::sqrt(changeSq));
  }
  const Vec3 goal = sub(current, change);
  const Vec3 temp = mul(add(*velocity, mul(change, omega)), dt);
  *velocity = mul(sub(*velocity, mul(temp, omega)), decay);
  Vec3 output = add(goal, mul(add(change, temp), decay));
  if (dot(sub(target, current), sub(output, target)) > 0.0F) {
    output = target;
    *velocity = Vec3(0.0F, 0.0F, 0.0F);
  }
  return output;
}

namespace detail {

/// Past this many radians wrap_angle reduces with fmod first, so its
/// correction loop runs at most once; below it the loop alone runs,
/// subtracting whole turns one at a time as the hinge joint always did.
constexpr float kWrapLoopLimit = 64.0F * kDetTwoPi;

} // namespace detail

/// The angle equal to `radians` modulo a turn, in [-pi, pi]; NaN and
/// infinities give NaN.
inline float wrap_angle(float radians) noexcept {
  if (!std::isfinite(radians)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  if (std::fabs(radians) > detail::kWrapLoopLimit) {
    radians = std::fmod(radians, kDetTwoPi);
  }
  while (radians > kDetPi) {
    radians -= kDetTwoPi;
  }
  while (radians < -kDetPi) {
    radians += kDetTwoPi;
  }
  return radians;
}

/// The signed shortest turn from `from` to `to`, in [-pi, pi].
inline float delta_angle(float from, float to) noexcept {
  return wrap_angle(to - from);
}

/// Interpolates between two angles the short way round. Not clamped.
inline float lerp_angle(float from, float to, float t) noexcept {
  return from + (delta_angle(from, to) * t);
}

/// move_towards for angles: turns `current` towards `target` the short way
/// round by at most `maxDelta` radians.
inline float move_towards_angle(float current, float target,
                                float maxDelta) noexcept {
  const float delta = delta_angle(current, target);
  if (std::fabs(delta) <= maxDelta) {
    return current + delta;
  }
  return current + ((delta > 0.0F) ? maxDelta : -maxDelta);
}

/// `value` wrapped into [0, length); 0 when length is not positive.
inline float repeat(float value, float length) noexcept {
  if (!(length > 0.0F)) {
    return 0.0F;
  }
  const float wrapped = value - (std::floor(value / length) * length);
  // Rounding can land a value just below a multiple on `length` itself.
  return (wrapped < length) ? wrapped : 0.0F;
}

/// `value` bouncing between 0 and `length`: 0 at 0, length at length,
/// 0 again at 2 length.
inline float ping_pong(float value, float length) noexcept {
  if (!(length > 0.0F)) {
    return 0.0F;
  }
  const float cycle = repeat(value, 2.0F * length);
  return length - std::fabs(cycle - length);
}

} // namespace engine::math
