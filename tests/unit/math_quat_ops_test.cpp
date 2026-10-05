// Tests the quaternion operations beyond the basics (engine/math/quat.h):
// inverse and relative rotation, nlerp, the axis accessors, angle, from_to,
// rotate_towards, integration, rotation vectors, swing-twist and Euler
// angles in every order. Exact at the identity and at half a turn where
// the arithmetic allows it, invariants elsewhere, and integrate is pinned
// bit for bit to the physics step it moved from.

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../test_harness.h"
#include "engine/math/quat.h"

namespace {

namespace math = engine::math;
using math::Quat;
using math::Vec3;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

/// A unit quaternion's components are a few ulp from exact after a
/// product or two of det_sin/det_cos values: 2e-6 is about 16 ulp of 1.
constexpr float kTolerance = 2.0e-6F;

bool near(float a, float b) noexcept {
  return std::fabs(a - b) <= kTolerance;
}

bool near(const Vec3 &a, const Vec3 &b) noexcept {
  return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

/// The same rotation, q or -q.
bool same_rotation(const Quat &a, const Quat &b) noexcept {
  return std::fabs(std::fabs(math::dot(a, b)) - 1.0F) <= kTolerance;
}

bool exactly(const Vec3 &v, float x, float y, float z) noexcept {
  return (v.x == x) && (v.y == y) && (v.z == z);
}

void check_inverse_and_relative() noexcept {
  const Quat q = math::from_axis_angle(Vec3(1.0F, 2.0F, 3.0F), 0.7F);
  const Quat scaled(q.x * 2.0F, q.y * 2.0F, q.z * 2.0F, q.w * 2.0F);
  check(same_rotation(math::mul(scaled, math::inverse(scaled)), Quat()),
        "a quaternion times its inverse is identity, at any scale");
  const Quat zero(0.0F, 0.0F, 0.0F, 0.0F);
  const Quat fromZero = math::inverse(zero);
  check((fromZero.w == 1.0F) && (fromZero.x == 0.0F),
        "a zero quaternion's inverse is identity");

  const Vec3 v(0.3F, -1.2F, 2.5F);
  check(near(math::rotate_inverse(math::rotate_vector(v, q), q), v),
        "rotate_inverse undoes rotate_vector");
  const Quat b = math::from_axis_angle(Vec3(0.0F, 1.0F, 0.0F), -1.1F);
  check(same_rotation(math::mul(q, math::relative(q, b)), b),
        "a rotation composed with its relative reaches the other");
}

void check_nlerp() noexcept {
  const Quat a = math::from_axis_angle(Vec3(0.0F, 1.0F, 0.0F), 0.2F);
  const Quat b = math::from_axis_angle(Vec3(0.0F, 1.0F, 0.0F), 1.4F);
  check(same_rotation(math::nlerp(a, b, 0.0F), a) &&
            same_rotation(math::nlerp(a, b, 1.0F), b),
        "nlerp starts and ends at its endpoints");
  const Quat negated(-b.x, -b.y, -b.z, -b.w);
  const Quat viaShort = math::nlerp(a, b, 0.5F);
  const Quat viaNegated = math::nlerp(a, negated, 0.5F);
  check((viaShort.x == viaNegated.x) && (viaShort.y == viaNegated.y) &&
            (viaShort.z == viaNegated.z) && (viaShort.w == viaNegated.w),
        "nlerp takes the short arc whichever sign the target has");
  check(near(math::angle(a, viaShort), 0.6F),
        "nlerp halfway between symmetric rotations is the middle one");
}

void check_axes_and_angle() noexcept {
  const Quat identity;
  check(exactly(math::forward(identity), 0.0F, 0.0F, -1.0F) &&
            exactly(math::back(identity), 0.0F, 0.0F, 1.0F) &&
            exactly(math::right(identity), 1.0F, 0.0F, 0.0F) &&
            exactly(math::up(identity), 0.0F, 1.0F, 0.0F),
        "identity faces -Z with +X right and +Y up, exactly");
  const Quat turned(0.0F, 1.0F, 0.0F, 0.0F);
  check(exactly(math::forward(turned), 0.0F, 0.0F, 1.0F) &&
            exactly(math::right(turned), -1.0F, 0.0F, 0.0F),
        "half a turn about Y faces +Z, exactly");
  check(math::angle(identity, identity) == 0.0F,
        "a rotation is no angle from itself, exactly");
  check(near(math::angle(identity, turned), math::kDetPi),
        "half a turn is pi");
  check(math::angle(turned, Quat(0.0F, -1.0F, 0.0F, 0.0F)) == 0.0F,
        "q and -q are the same rotation");
  check(near(math::angle(identity,
                         math::from_axis_angle(Vec3(1.0F, 1.0F, 0.0F), 1.0F)),
             1.0F),
        "angle measures the turn between two rotations");
}

void check_from_to_and_rotate_towards() noexcept {
  const Vec3 from(1.0F, 0.0F, 0.0F);
  const Vec3 to(0.0F, 2.0F, 2.0F);
  const Quat q = math::from_to(from, to);
  check(near(math::rotate_vector(from, q),
             Vec3(0.0F, 0.70710678F, 0.70710678F)),
        "from_to turns one direction onto the other");
  const Quat same = math::from_to(Vec3(0.0F, 3.0F, 0.0F),
                                  Vec3(0.0F, 1.0F, 0.0F));
  check((same.x == 0.0F) && (same.y == 0.0F) && (same.z == 0.0F) &&
            (same.w == 1.0F),
        "parallel directions give identity, exactly");
  const Quat opposite =
      math::from_to(Vec3(0.0F, 0.0F, 1.0F), Vec3(0.0F, 0.0F, -1.0F));
  check(near(math::rotate_vector(Vec3(0.0F, 0.0F, 1.0F), opposite),
             Vec3(0.0F, 0.0F, -1.0F)) &&
            (opposite.w == 0.0F),
        "opposite directions turn half a turn");
  const Quat none = math::from_to(Vec3(0.0F, 0.0F, 0.0F), to);
  check(none.w == 1.0F, "a zero direction gives identity");

  const Quat target = math::from_axis_angle(Vec3(0.0F, 1.0F, 0.0F), 1.5F);
  const Quat step = math::rotate_towards(Quat(), target, 0.5F);
  check(near(math::angle(Quat(), step), 0.5F) &&
            near(math::angle(step, target), 1.0F),
        "rotate_towards turns by the bound along the way");
  const Quat landed = math::rotate_towards(Quat(), target, 2.0F);
  check((landed.x == target.x) && (landed.y == target.y) &&
            (landed.z == target.z) && (landed.w == target.w),
        "rotate_towards lands on a target within reach exactly");
}

/// The physics step's integration before it moved to math::integrate.
Quat physics_step_integration(const Quat &rotation, const Vec3 &omega,
                              float dt) noexcept {
  const float angSpeedSq = math::length_sq(omega);
  if (angSpeedSq > 1e-12F) {
    const float angSpeed = std::sqrt(angSpeedSq);
    const Vec3 axis = math::div(omega, angSpeed);
    const Quat deltaRot = math::from_axis_angle(axis, angSpeed * dt);
    return math::normalize(math::mul(deltaRot, rotation));
  }
  return rotation;
}

void check_integration_and_rotation_vectors() noexcept {
  bool bitEqual = true;
  Quat a = math::from_axis_angle(Vec3(0.2F, 1.0F, -0.4F), 0.3F);
  Quat b = a;
  for (int i = 0; i < 2000; ++i) {
    const float t = static_cast<float>(i) * 0.01F;
    const Vec3 omega(std::sin(t) * 3.0F, std::cos(t * 1.7F) * 2.0F,
                     (i % 7 == 0) ? 0.0F : 0.5F);
    a = math::integrate(a, omega, 1.0F / 60.0F);
    b = physics_step_integration(b, omega, 1.0F / 60.0F);
    bitEqual = bitEqual && (std::memcmp(&a, &b, sizeof(Quat)) == 0);
  }
  check(bitEqual, "integrate is the physics step's integration, bit for bit");
  const Quat still = math::integrate(a, Vec3(0.0F, 0.0F, 0.0F), 1.0F);
  check(std::memcmp(&still, &a, sizeof(Quat)) == 0,
        "no angular velocity leaves the rotation's bits alone");
  check(same_rotation(math::integrate(Quat(), Vec3(0.0F, math::kDetPi, 0.0F),
                                      1.0F),
                      Quat(0.0F, 1.0F, 0.0F, 0.0F)),
        "pi rad/s for a second about Y is half a turn");

  const Vec3 rotation(0.4F, -0.9F, 0.2F);
  check(near(math::to_rotation_vector(math::from_rotation_vector(rotation)),
             rotation),
        "rotation vectors round-trip");
  check(exactly(math::to_rotation_vector(Quat()), 0.0F, 0.0F, 0.0F),
        "identity has a zero rotation vector");
  const Quat longWay = math::from_axis_angle(Vec3(0.0F, 0.0F, 1.0F), 5.0F);
  check(math::length(math::to_rotation_vector(longWay)) <=
            math::kDetPi + kTolerance,
        "a rotation vector takes the shorter arc");
  const Quat tiny = math::from_rotation_vector(Vec3(1.0e-7F, 0.0F, 0.0F));
  check(tiny.w == 1.0F, "a rotation vector below 1e-6 rad is identity");
}

void check_swing_twist() noexcept {
  const Vec3 axis(0.0F, 1.0F, 0.0F);
  const Quat twistIn = math::from_axis_angle(axis, 0.8F);
  const Quat swingIn = math::from_axis_angle(Vec3(1.0F, 0.0F, 0.5F), 0.6F);
  const Quat q = math::mul(swingIn, twistIn);
  Quat swing;
  Quat twist;
  math::swing_twist(q, axis, &swing, &twist);
  check(same_rotation(math::mul(swing, twist), q),
        "swing after twist rebuilds the rotation");
  check((twist.x == 0.0F) && (twist.z == 0.0F),
        "the twist turns about the axis only");
  check(near(math::dot(Vec3(swing.x, swing.y, swing.z), axis), 0.0F),
        "the swing's axis is perpendicular to the twist axis");
  Quat pureSwing;
  Quat pureTwist;
  math::swing_twist(twistIn, axis, &pureSwing, &pureTwist);
  check(same_rotation(pureSwing, Quat()) && same_rotation(pureTwist, twistIn),
        "a rotation about the axis is all twist");
  math::swing_twist(Quat(1.0F, 0.0F, 0.0F, 0.0F), axis, &pureSwing,
                    &pureTwist);
  check((pureTwist.w == 1.0F) &&
            same_rotation(pureSwing, Quat(1.0F, 0.0F, 0.0F, 0.0F)),
        "half a turn across the axis has no defined twist: identity");
}

void check_euler_orders() noexcept {
  const math::EulerOrder orders[] = {
      math::EulerOrder::XYZ, math::EulerOrder::XZY, math::EulerOrder::YXZ,
      math::EulerOrder::YZX, math::EulerOrder::ZXY, math::EulerOrder::ZYX};
  bool roundTrips = true;
  bool defaultMatches = true;
  // Steps of pi/40 reach both poles of the middle axis exactly.
  for (int i = -20; i <= 20; i += 2) {
    for (int j = -38; j <= 38; j += 7) {
      for (int k = -38; k <= 38; k += 9) {
        const float pitch = static_cast<float>(i) * (math::kDetPi / 40.0F);
        const float yaw = static_cast<float>(j) * (math::kDetPi / 40.0F);
        const float roll = static_cast<float>(k) * (math::kDetPi / 40.0F);
        for (const math::EulerOrder order : orders) {
          const Quat q = math::from_euler(pitch, yaw, roll, order);
          float p = 0.0F;
          float y = 0.0F;
          float r = 0.0F;
          roundTrips = roundTrips && math::to_euler(q, order, &p, &y, &r) &&
                       same_rotation(math::from_euler(p, y, r, order), q);
        }
        const Quat q = math::from_euler(pitch, yaw, roll);
        const Quat viaOrder =
            math::from_euler(pitch, yaw, roll, math::EulerOrder::YXZ);
        float angles[3] = {};
        float viaOrderAngles[3] = {};
        static_cast<void>(
            math::to_euler(q, &angles[0], &angles[1], &angles[2]));
        static_cast<void>(math::to_euler(q, math::EulerOrder::YXZ,
                                         &viaOrderAngles[0],
                                         &viaOrderAngles[1],
                                         &viaOrderAngles[2]));
        defaultMatches = defaultMatches &&
                         (std::memcmp(&q, &viaOrder, sizeof(Quat)) == 0) &&
                         (std::memcmp(angles, viaOrderAngles,
                                      sizeof(angles)) == 0);
      }
    }
  }
  check(roundTrips,
        "every order round-trips, through both poles of its middle axis");
  check(defaultMatches, "the default order is YXZ, bit for bit");

  const Quat xyz = math::from_euler(0.3F, -0.5F, 0.9F, math::EulerOrder::XYZ);
  const Quat composed =
      math::mul(math::mul(math::from_axis_angle(Vec3(1.0F, 0.0F, 0.0F), 0.3F),
                          math::from_axis_angle(Vec3(0.0F, 1.0F, 0.0F), -0.5F)),
                math::from_axis_angle(Vec3(0.0F, 0.0F, 1.0F), 0.9F));
  check(same_rotation(xyz, composed),
        "XYZ composes pitch, then yaw, then roll, left to right");
  check(!same_rotation(xyz, math::from_euler(0.3F, -0.5F, 0.9F,
                                             math::EulerOrder::ZYX)),
        "a different order is a different rotation");
}

} // namespace

/// Runs the quaternion operations suite.
int main() {
  check_inverse_and_relative();
  check_nlerp();
  check_axes_and_angle();
  check_from_to_and_rotate_towards();
  check_integration_and_rotation_vectors();
  check_swing_twist();
  check_euler_orders();
  return g_tests.finish("math_quat_ops_test");
}
