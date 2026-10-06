// Tests the interpolation and smoothing owner (engine/math/interpolation.h):
// exact endpoints and fixed points where the arithmetic is exact, and
// invariants elsewhere -- no overshoot, monotonic steps, convergence that
// does not depend on how a span of time is split into steps, angles kept
// in [-pi, pi] -- plus bit equality between wrap_angle and the loop the
// hinge joint carried before it moved here.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "../test_harness.h"
#include "engine/math/interpolation.h"

namespace {

namespace math = engine::math;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

std::uint32_t bits(float value) noexcept {
  std::uint32_t out = 0U;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

void check_scalar_blends() noexcept {
  check(math::lerp(2.0F, 6.0F, 0.0F) == 2.0F, "lerp is a at t = 0");
  check(math::lerp(2.0F, 6.0F, 1.0F) == 6.0F, "lerp is b at t = 1");
  check(math::lerp(2.0F, 6.0F, 0.5F) == 4.0F, "lerp halves");
  check(math::lerp(2.0F, 6.0F, 2.0F) == 10.0F, "lerp extrapolates");
  check(math::inverse_lerp(2.0F, 6.0F, 4.0F) == 0.5F,
        "inverse_lerp undoes lerp");
  check(math::inverse_lerp(3.0F, 3.0F, 9.0F) == 0.0F,
        "inverse_lerp of an empty range is 0");
  check(math::remap(5.0F, 0.0F, 10.0F, 100.0F, 200.0F) == 150.0F,
        "remap maps the midpoint to the midpoint");
  check((math::saturate(-1.0F) == 0.0F) && (math::saturate(2.0F) == 1.0F) &&
            (math::saturate(0.25F) == 0.25F),
        "saturate clamps to [0, 1]");
  check(math::saturate(std::numeric_limits<float>::quiet_NaN()) == 0.0F,
        "saturate of NaN is 0");

  check((math::smoothstep(1.0F, 3.0F, 0.0F) == 0.0F) &&
            (math::smoothstep(1.0F, 3.0F, 1.0F) == 0.0F) &&
            (math::smoothstep(1.0F, 3.0F, 3.0F) == 1.0F) &&
            (math::smoothstep(1.0F, 3.0F, 9.0F) == 1.0F),
        "smoothstep is 0 below its edges and 1 above");
  check(math::smoothstep(1.0F, 3.0F, 2.0F) == 0.5F,
        "smoothstep is one half halfway");
  check((math::smoothstep(2.0F, 2.0F, 1.0F) == 0.0F) &&
            (math::smoothstep(2.0F, 2.0F, 2.0F) == 1.0F),
        "smoothstep with equal edges is a step at the edge");
  bool monotonic = true;
  float previous = 0.0F;
  for (int i = 0; i <= 1000; ++i) {
    const float value =
        math::smoothstep(0.0F, 1.0F, static_cast<float>(i) / 1000.0F);
    monotonic = monotonic && (value >= previous);
    previous = value;
  }
  check(monotonic, "smoothstep never decreases");
}

void check_move_towards() noexcept {
  check(math::move_towards(0.0F, 10.0F, 3.0F) == 3.0F,
        "move_towards steps by the bound");
  check(math::move_towards(9.0F, 10.0F, 3.0F) == 10.0F,
        "move_towards lands on the target, never past it");
  check(math::move_towards(10.0F, 0.0F, 3.0F) == 7.0F,
        "move_towards steps downwards too");
  check(math::move_towards(5.0F, 6.0F, -1.0F) == 4.0F,
        "a negative bound moves away");

  const math::Vec3 from(0.0F, 0.0F, 0.0F);
  const math::Vec3 to(3.0F, 4.0F, 0.0F);
  const math::Vec3 step = math::move_towards(from, to, 1.0F);
  check(std::fabs(math::length(step) - 1.0F) <= 1.0e-6F,
        "a point moves the bound's distance");
  check(std::fabs(math::length(math::move_towards(step, to, 4.0F)) - 5.0F) <=
            1.0e-6F,
        "and keeps to the line towards its target");
  const math::Vec3 landed = math::move_towards(from, to, 5.0F);
  check((landed.x == 3.0F) && (landed.y == 4.0F) && (landed.z == 0.0F),
        "a point within reach lands on its target exactly");
  const math::Vec3 same = math::move_towards(to, to, 0.0F);
  check((same.x == 3.0F) && (same.y == 4.0F),
        "a point already there stays");
}

void check_exp_decay() noexcept {
  check((math::exp_decay_factor(0.0F, 1.0F) == 0.0F) &&
            (math::exp_decay_factor(2.0F, 0.0F) == 0.0F) &&
            (math::exp_decay_factor(-1.0F, 1.0F) == 0.0F),
        "no rate or no time covers nothing");
  check(math::exp_decay_factor(1000.0F, 1.0F) == 1.0F,
        "a long enough time covers everything");
  // e^-1 by det_exp is within a few ulp of libm's; 1e-6 is about 8 ulp.
  check(std::fabs(math::exp_decay_factor(2.0F, 0.5F) -
                  (1.0F - std::exp(-1.0F))) <= 1.0e-6F,
        "the factor is 1 - e^(-rate dt)");

  // The law the spring arm moved to: one step of 0.2 s and ten of 0.02 s
  // close the same share of the gap. Each step rounds once or twice, so
  // ten steps differ by tens of ulp of the 4 m gap at most: 1e-5 m.
  float coarse = math::exp_decay(1.0F, 5.0F, 2.0F, 0.2F);
  float fine = 1.0F;
  for (int i = 0; i < 10; ++i) {
    fine = math::exp_decay(fine, 5.0F, 2.0F, 0.02F);
  }
  check(std::fabs(coarse - fine) <= 1.0e-5F,
        "exp_decay converges at the same rate whatever the step size");
  check((coarse > 1.0F) && (coarse < 5.0F),
        "and stays between where it was and its target");
  const math::Vec3 point =
      math::exp_decay(math::Vec3(0.0F, 0.0F, 0.0F),
                      math::Vec3(2.0F, 0.0F, 0.0F), 1000.0F, 1.0F);
  check(point.x == 2.0F, "a point decays onto its target");
}

void check_smooth_damp() noexcept {
  float velocity = 0.0F;
  float value = 0.0F;
  bool overshot = false;
  bool monotonic = true;
  for (int i = 0; i < 600; ++i) {
    const float next =
        math::smooth_damp(value, 10.0F, &velocity, 0.3F,
                          std::numeric_limits<float>::infinity(), 1.0F / 60.0F);
    overshot = overshot || (next > 10.0F);
    monotonic = monotonic && (next >= value);
    value = next;
  }
  check(!overshot, "smooth_damp never passes its target");
  check(monotonic, "and approaches it without turning back");
  check(std::fabs(value - 10.0F) <= 1.0e-4F, "and arrives");

  // With the speed capped at 1, a second from 100 m away covers about a
  // metre; uncapped, the spring covers most of the gap.
  velocity = 0.0F;
  value = 0.0F;
  for (int i = 0; i < 60; ++i) {
    value = math::smooth_damp(value, 100.0F, &velocity, 0.3F, 1.0F,
                              1.0F / 60.0F);
  }
  check((value > 0.0F) && (value < 1.5F), "maxSpeed caps the speed");

  velocity = 3.0F;
  check((math::smooth_damp(4.0F, 9.0F, &velocity, 0.3F, 1.0F, 0.0F) ==
         4.0F) &&
            (velocity == 3.0F),
        "no time leaves the value and its speed alone");
  check(math::smooth_damp(4.0F, 9.0F, nullptr, 0.3F, 1.0F, 0.1F) == 4.0F,
        "no velocity slot leaves the value alone");

  math::Vec3 point(0.0F, 0.0F, 0.0F);
  math::Vec3 speed(0.0F, 0.0F, 0.0F);
  const math::Vec3 target(3.0F, 0.0F, 4.0F);
  bool passed = false;
  for (int i = 0; i < 600; ++i) {
    point = math::smooth_damp(point, target, &speed, 0.3F,
                              std::numeric_limits<float>::infinity(),
                              1.0F / 60.0F);
    passed = passed || (math::dot(math::sub(target, point),
                                  math::Vec3(3.0F, 0.0F, 4.0F)) < 0.0F);
  }
  check(!passed, "a point's spring never passes its target");
  check(math::distance(point, target) <= 1.0e-4F, "and arrives");
}

/// The loop hinge_joint.cpp carried before wrap_angle replaced it.
float hinge_wrap(float angle) noexcept {
  constexpr float kHingePi = 3.14159265F;
  constexpr float kHingeTwoPi = 6.28318531F;
  while (angle > kHingePi) {
    angle -= kHingeTwoPi;
  }
  while (angle < -kHingePi) {
    angle += kHingeTwoPi;
  }
  return angle;
}

void check_angles() noexcept {
  const float pi = math::kDetPi;
  check(math::wrap_angle(0.5F) == 0.5F, "an angle in range is unchanged");
  check((math::wrap_angle(pi) == pi) && (math::wrap_angle(-pi) == -pi),
        "both ends of the range are kept");
  check(math::wrap_angle(math::kDetTwoPi) == 0.0F, "a whole turn is zero");
  bool inRange = true;
  bool sameAsHinge = true;
  for (int i = -4000; i <= 4000; ++i) {
    const float angle = static_cast<float>(i) * 0.1F;
    const float wrapped = math::wrap_angle(angle);
    inRange = inRange && (wrapped >= -pi) && (wrapped <= pi);
    sameAsHinge = sameAsHinge && (bits(wrapped) == bits(hinge_wrap(angle)));
  }
  check(inRange, "wrapped angles lie in [-pi, pi]");
  check(sameAsHinge,
        "within 64 turns the wrap is the hinge joint's, bit for bit");
  const float huge = math::wrap_angle(1.0e7F);
  check((huge >= -pi) && (huge <= pi), "a huge angle still wraps");
  check(std::isnan(math::wrap_angle(std::numeric_limits<float>::infinity())) &&
            std::isnan(
                math::wrap_angle(std::numeric_limits<float>::quiet_NaN())),
        "infinity and NaN have no angle");

  check(std::fabs(math::delta_angle(0.1F, math::kDetTwoPi - 0.1F) + 0.2F) <=
            1.0e-6F,
        "delta_angle turns the short way across zero");
  check(std::fabs(math::lerp_angle(pi - 0.1F, -pi + 0.1F, 0.5F) - pi) <=
            1.0e-6F,
        "lerp_angle crosses pi rather than going round");
  check(std::fabs(math::move_towards_angle(pi - 0.1F, -pi + 0.1F, 0.05F) -
                  (pi - 0.05F)) <= 1.0e-6F,
        "move_towards_angle turns the short way");
  check(math::move_towards_angle(1.0F, 1.2F, 0.5F) == 1.2F,
        "move_towards_angle lands on a target within reach");

  check(math::repeat(5.5F, 2.0F) == 1.5F, "repeat wraps above");
  check(math::repeat(-0.5F, 2.0F) == 1.5F, "repeat wraps below zero");
  check(math::repeat(4.0F, 2.0F) == 0.0F, "repeat of a multiple is 0");
  check(math::repeat(1.0F, 0.0F) == 0.0F, "repeat of no length is 0");
  check((math::ping_pong(0.0F, 2.0F) == 0.0F) &&
            (math::ping_pong(2.0F, 2.0F) == 2.0F) &&
            (math::ping_pong(3.0F, 2.0F) == 1.0F) &&
            (math::ping_pong(4.0F, 2.0F) == 0.0F),
        "ping_pong rises to its length and falls back");
}

} // namespace

/// Runs the interpolation suite.
int main() {
  check_scalar_blends();
  check_move_towards();
  check_exp_decay();
  check_smooth_damp();
  check_angles();
  return g_tests.finish("math_interpolation_test");
}
