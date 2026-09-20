// Pins the joint projections' behaviour on rank-deficient anchor and
// inertia matrices: a body that can rotate about one world axis only (a
// rank-one tensor rotated off the world axes) or about two (rank two) has
// exactly the reachable component of a relative angular velocity removed
// and the unreachable one left alone, and the whole solve is invariant
// under a rotation of the entire setup. The rotated rank-one case is the
// one a diagonal-only fallback gets wrong: it doubles the correction and
// flips the body's spin.

#include <cmath>
#include <cstdio>

#include "../test_harness.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "joint_projection.h"

namespace {

namespace math = engine::math;
namespace physics = engine::physics;

constexpr float kTolerance = 1.0e-4F;

bool near(const math::Vec3 &a, const math::Vec3 &b) noexcept {
  return math::length(math::sub(a, b)) <= kTolerance;
}

/// Body B free about its local X only, rotated by `rotation`; body A a
/// locked static anchor.
struct Setup final {
  physics::Transform tA{};
  physics::Transform tB{};
  physics::RigidBody bodyA{};
  physics::RigidBody bodyB{};
  physics::JointSolveContext ctx{};

  explicit Setup(const math::Quat &rotation, const math::Vec3 &inverseInertiaB,
                 const math::Vec3 &angularVelocityB) noexcept {
    tB.rotation = rotation;
    bodyB.angularVelocity = angularVelocityB;
    bodyA.inverseMass = 0.0F;
    bodyB.inverseMass = 1.0F;
    ctx.tA = &tA;
    ctx.tB = &tB;
    ctx.bodyA = &bodyA;
    ctx.bodyB = &bodyB;
    ctx.invMassA = 0.0F;
    ctx.invMassB = 1.0F;
    ctx.invInertiaA = math::Vec3(0.0F, 0.0F, 0.0F);
    ctx.invInertiaB = inverseInertiaB;
  }
};

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  const math::Quat aboutZ45 =
      math::from_axis_angle(math::Vec3(0.0F, 0.0F, 1.0F), 0.25F * 3.14159265F);
  const float c = std::sqrt(0.5F);
  // The one world axis body B can spin about: local X rotated 45 degrees.
  const math::Vec3 axis(c, c, 0.0F);

  // --- Rank one, rotated: the reachable spin is removed, not doubled ---
  {
    Setup s(aboutZ45, math::Vec3(1.0F, 0.0F, 0.0F), axis);
    physics::project_relative_angular_velocity(s.ctx, axis);
    ctx.check(near(s.bodyB.angularVelocity, math::Vec3(0.0F, 0.0F, 0.0F)),
              "rank-one rotated: the spin about the free axis is removed");
  }
  {
    // A component the body cannot have (about the locked plane) stays.
    const math::Vec3 locked(-c, c, 0.0F);
    Setup s(aboutZ45, math::Vec3(1.0F, 0.0F, 0.0F), math::add(axis, locked));
    physics::project_relative_angular_velocity(s.ctx, math::add(axis, locked));
    ctx.check(near(s.bodyB.angularVelocity, locked),
              "rank-one rotated: the unreachable component is untouched");
  }

  // --- Rank two, rotated: the free plane is removed, the locked axis kept ---
  {
    const math::Quat tilt = math::normalize(
        math::mul(math::from_axis_angle(math::Vec3(1.0F, 0.0F, 0.0F), 0.6F),
                  math::from_axis_angle(math::Vec3(0.0F, 0.0F, 1.0F), 0.9F)));
    const math::Vec3 freeA = math::rotate_vector(math::Vec3(1.0F, 0.0F, 0.0F), tilt);
    const math::Vec3 freeB = math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), tilt);
    const math::Vec3 lockedAxis =
        math::rotate_vector(math::Vec3(0.0F, 0.0F, 1.0F), tilt);
    const math::Vec3 spin = math::add(math::add(math::mul(freeA, 0.7F),
                                                math::mul(freeB, -0.4F)),
                                      math::mul(lockedAxis, 0.3F));
    Setup s(tilt, math::Vec3(2.0F, 0.5F, 0.0F), spin);
    physics::project_relative_angular_velocity(s.ctx, spin);
    ctx.check(near(s.bodyB.angularVelocity, math::mul(lockedAxis, 0.3F)),
              "rank-two rotated: the free plane is removed, the locked axis kept");
  }

  // --- Whole-setup rotational invariance ---
  {
    const math::Quat frame = math::normalize(
        math::mul(math::from_axis_angle(math::Vec3(0.0F, 1.0F, 0.0F), 1.1F),
                  math::from_axis_angle(math::Vec3(1.0F, 0.0F, 0.0F), -0.4F)));
    const math::Vec3 spin(0.3F, 0.8F, -0.2F);
    Setup plain(aboutZ45, math::Vec3(1.0F, 0.0F, 0.0F), spin);
    physics::project_relative_angular_velocity(plain.ctx, spin);
    Setup rotated(math::normalize(math::mul(frame, aboutZ45)),
                  math::Vec3(1.0F, 0.0F, 0.0F),
                  math::rotate_vector(spin, frame));
    physics::project_relative_angular_velocity(rotated.ctx,
                                               math::rotate_vector(spin, frame));
    ctx.check(near(rotated.bodyB.angularVelocity,
                   math::rotate_vector(plain.bodyB.angularVelocity, frame)),
              "rotating the whole setup rotates the result");
  }

  // --- The anchor velocity projection on a locked-axis pair ---
  {
    // Both bodies locked about every axis: the anchor mass matrix is the
    // scalar inverse mass, definite, and the relative velocity is removed.
    Setup s(math::Quat(), math::Vec3(0.0F, 0.0F, 0.0F),
            math::Vec3(0.0F, 0.0F, 0.0F));
    s.bodyB.velocity = math::Vec3(1.0F, 0.0F, 0.0F);
    const math::Vec3 lever(0.0F, 1.0F, 0.0F);
    physics::project_point_velocity(s.ctx, lever, lever,
                                    math::Vec3(1.0F, 0.0F, 0.0F));
    ctx.check(near(s.bodyB.velocity, math::Vec3(0.0F, 0.0F, 0.0F)),
              "anchor velocity removed through a definite mass matrix");
  }

  return ctx.finish("joint_rank_deficient");
}
