// Verifies the H-05 joint constraint models against analytical cases: each
// joint type is driven through the full fixed-step pipeline (integration,
// resolve, constraint solve) and asserted against its closed-form
// equilibrium or limit configuration, including a two-link pendulum
// settling to the analytic rest pose. Settling tests clear the sleep state
// every step so the solver, not the sleep policy, produces the rest pose.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/core/cvar.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/constraint_solver.h"
#include "engine/physics/physics.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

using World = engine::runtime::World;
using Entity = engine::runtime::Entity;
using Transform = engine::runtime::Transform;
using RigidBody = engine::runtime::RigidBody;
namespace math = engine::math;
namespace physics = engine::physics;

constexpr float kDt = 1.0F / 60.0F;

// Helper: dynamic collider-free body so contacts and CCD never interfere.
Entity make_free_body(World &w, const math::Vec3 &pos, const math::Quat &rot,
                      float invMass, float invInertia) noexcept {
  const Entity e = w.create_entity();
  Transform t{};
  t.position = pos;
  t.rotation = rot;
  w.add_transform(e, t);

  RigidBody rb{};
  rb.inverseMass = invMass;
  rb.inverseInertia = invInertia;
  w.add_rigid_body(e, rb);
  return e;
}

// Helper: one full fixed step with the sleep state cleared so settling is
// produced by the constraint solver alone.
bool step_world(World &w, Entity *bodies, int count) noexcept {
  for (int i = 0; i < count; ++i) {
    RigidBody *rb = w.get_rigid_body_ptr(bodies[i]);
    if (rb != nullptr) {
      rb->sleeping = false;
      rb->sleepFrameCount = 0U;
    }
  }
  w.begin_update_phase();
  const bool ok = engine::runtime::step_physics(w, kDt) &&
                  engine::runtime::resolve_collisions(w, kDt);
  w.commit_update_phase();
  w.begin_render_prep_phase();
  w.end_frame_phase();
  return ok;
}

float quat_alignment(const math::Quat &a, const math::Quat &b) noexcept {
  return std::fabs((a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w));
}

// Fresh world with gravity off and the solver iteration count pinned so
// the analytic assertions hold for an exact configuration: 16 iterations
// bounds the articulated chain's steady-state sag under gravity within
// the asserted tolerances.
std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
    engine::core::cvar_set_int("physics.solver_iterations", 16);
    engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);
  }
  return world;
}

// ---- Hinge: limits are radians about the hinge axis ------------------------

int test_hinge_limit_clamps_twist_in_radians() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 1;
  }

  Entity bodies[2] = {
      make_free_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), math::Quat(),
                     0.0F, 0.0F),
      make_free_body(*world, math::Vec3(1.0F, 0.0F, 0.0F), math::Quat(),
                     1.0F, 1.0F)};

  const math::Vec3 axis(0.0F, 0.0F, 1.0F);
  const physics::JointId jid = physics::add_hinge_joint(
      *world, bodies[0], bodies[1], math::Vec3(1.0F, 0.0F, 0.0F), axis);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }
  physics::set_joint_limits(*world, jid, -0.5F, 0.5F);

  {
    RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
    if (rb == nullptr) {
      return 3;
    }
    rb->angularVelocity = math::Vec3(0.0F, 0.0F, 5.0F);
  }

  for (int i = 0; i < 60; ++i) {
    if (!step_world(*world, bodies, 2)) {
      return 4;
    }
  }

  Transform tB{};
  world->get_transform(bodies[1], &tB);
  math::Vec3 twistAxis{};
  float twist = 0.0F;
  math::to_axis_angle(tB.rotation, &twistAxis, &twist);
  const float signedTwist = (twistAxis.z < 0.0F) ? -twist : twist;
  if (std::fabs(signedTwist - 0.5F) > 1e-3F) {
    std::printf("FAIL hinge_limit: twist=%.5f expected 0.5\n",
                static_cast<double>(signedTwist));
    return 5;
  }

  const RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
  if ((rb == nullptr) || (math::length(rb->angularVelocity) > 1e-3F)) {
    return 6;
  }
  if (std::fabs(tB.position.x - 1.0F) > 1e-3F ||
      std::fabs(tB.position.y) > 1e-3F || std::fabs(tB.position.z) > 1e-3F) {
    return 7;
  }
  return 0;
}

// ---- Hinge: off-axis rotation is constrained away --------------------------

int test_hinge_realigns_axes_after_disturbance() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 1;
  }

  Entity bodies[2] = {
      make_free_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), math::Quat(),
                     0.0F, 0.0F),
      make_free_body(*world, math::Vec3(1.0F, 0.0F, 0.0F), math::Quat(),
                     1.0F, 1.0F)};

  const physics::JointId jid = physics::add_hinge_joint(
      *world, bodies[0], bodies[1], math::Vec3(1.0F, 0.0F, 0.0F),
      math::Vec3(0.0F, 0.0F, 1.0F));
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  {
    RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
    if (rb == nullptr) {
      return 3;
    }
    rb->angularVelocity = math::Vec3(4.0F, 0.0F, 0.0F);
  }

  for (int i = 0; i < 60; ++i) {
    if (!step_world(*world, bodies, 2)) {
      return 4;
    }
  }

  Transform tB{};
  world->get_transform(bodies[1], &tB);
  const math::Vec3 worldAxisB =
      math::rotate_vector(math::Vec3(0.0F, 0.0F, 1.0F), tB.rotation);
  if ((std::fabs(worldAxisB.x) > 1e-3F) || (std::fabs(worldAxisB.y) > 1e-3F)) {
    std::printf("FAIL hinge_realign: axisB=(%.5f,%.5f,%.5f)\n",
                static_cast<double>(worldAxisB.x),
                static_cast<double>(worldAxisB.y),
                static_cast<double>(worldAxisB.z));
    return 5;
  }
  if (std::fabs(tB.position.x - 1.0F) > 1e-3F ||
      std::fabs(tB.position.y) > 1e-3F || std::fabs(tB.position.z) > 1e-3F) {
    return 6;
  }
  return 0;
}

// ---- Ball-socket: anchors rotate with the body (pendulum equilibrium) ------

int test_ball_socket_pendulum_rests_below_pivot() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 1;
  }
  engine::runtime::set_gravity(*world, 0.0F, -10.0F, 0.0F);

  const math::Vec3 pivot(0.0F, 1.0F, 0.0F);
  Entity bodies[2] = {
      make_free_body(*world, math::Vec3(0.0F, 2.0F, 0.0F), math::Quat(),
                     0.0F, 0.0F),
      make_free_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), math::Quat(),
                     1.0F, 1.0F)};

  const physics::JointId jid =
      physics::add_ball_socket_joint(*world, bodies[0], bodies[1], pivot);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  {
    RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
    if (rb == nullptr) {
      return 3;
    }
    rb->velocity = math::Vec3(2.0F, 0.0F, 0.0F);
  }

  for (int i = 0; i < 3000; ++i) {
    if (!step_world(*world, bodies, 2)) {
      return 4;
    }
    if (i == 30) {
      Transform mid{};
      world->get_transform(bodies[1], &mid);
      const math::Vec3 worldAnchor = math::add(
          mid.position,
          math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), mid.rotation));
      if (math::length(math::sub(worldAnchor, pivot)) > 2e-3F) {
        std::printf("FAIL ball_pendulum: mid-swing anchor error %.5f\n",
                    static_cast<double>(
                        math::length(math::sub(worldAnchor, pivot))));
        return 5;
      }
    }
  }

  Transform tB{};
  world->get_transform(bodies[1], &tB);
  if (math::length(tB.position) > 5e-3F) {
    std::printf("FAIL ball_pendulum: rest pos=(%.5f,%.5f,%.5f)\n",
                static_cast<double>(tB.position.x),
                static_cast<double>(tB.position.y),
                static_cast<double>(tB.position.z));
    return 6;
  }

  const RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
  if ((rb == nullptr) || (math::length(rb->velocity) > 5e-3F) ||
      (math::length(rb->angularVelocity) > 5e-3F)) {
    if (rb != nullptr) {
      std::printf("FAIL ball_pendulum: residual v=%.5f w=%.5f\n",
                  static_cast<double>(math::length(rb->velocity)),
                  static_cast<double>(math::length(rb->angularVelocity)));
    }
    return 7;
  }
  return 0;
}

// ---- Fixed: creation-time relative pose is restored ------------------------

int test_fixed_joint_preserves_creation_pose() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 1;
  }

  const math::Quat rotB =
      math::from_axis_angle(math::Vec3(0.0F, 1.0F, 0.0F), 0.5236F);
  Entity bodies[2] = {
      make_free_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), math::Quat(),
                     0.0F, 0.0F),
      make_free_body(*world, math::Vec3(1.0F, 0.5F, 0.0F), rotB, 1.0F, 1.0F)};

  const physics::JointId jid =
      physics::add_fixed_joint(*world, bodies[0], bodies[1]);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  {
    RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
    if (rb == nullptr) {
      return 3;
    }
    rb->velocity = math::Vec3(0.0F, 2.0F, 0.0F);
    rb->angularVelocity = math::Vec3(3.0F, 1.0F, 2.0F);
  }

  for (int i = 0; i < 120; ++i) {
    if (!step_world(*world, bodies, 2)) {
      return 4;
    }
  }

  Transform tB{};
  world->get_transform(bodies[1], &tB);
  const math::Vec3 posError =
      math::sub(tB.position, math::Vec3(1.0F, 0.5F, 0.0F));
  if (math::length(posError) > 1e-3F) {
    std::printf("FAIL fixed_pose: pos error %.5f\n",
                static_cast<double>(math::length(posError)));
    return 5;
  }
  if (quat_alignment(tB.rotation, rotB) < 1.0F - 1e-5F) {
    std::printf("FAIL fixed_pose: rot alignment %.7f\n",
                static_cast<double>(quat_alignment(tB.rotation, rotB)));
    return 6;
  }

  const RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
  if ((rb == nullptr) || (math::length(rb->velocity) > 1e-3F) ||
      (math::length(rb->angularVelocity) > 1e-3F)) {
    return 7;
  }
  return 0;
}

// ---- Slider: rail from body A's frame, rotation locked, radian-free limits -

int test_slider_rail_lock_and_travel_limits() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 1;
  }

  const math::Quat rotA =
      math::from_axis_angle(math::Vec3(0.0F, 0.0F, 1.0F), 1.5708F);
  Entity bodies[2] = {
      make_free_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), rotA, 0.0F, 0.0F),
      make_free_body(*world, math::Vec3(2.0F, 0.5F, 0.25F), math::Quat(),
                     1.0F, 1.0F)};

  const physics::JointId jid = physics::add_slider_joint(
      *world, bodies[0], bodies[1], math::Vec3(1.0F, 0.0F, 0.0F));
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  {
    RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
    if (rb == nullptr) {
      return 3;
    }
    rb->velocity = math::Vec3(0.0F, 1.0F, 1.0F);
  }

  for (int i = 0; i < 120; ++i) {
    if (!step_world(*world, bodies, 2)) {
      return 4;
    }
  }

  Transform tB{};
  world->get_transform(bodies[1], &tB);
  if ((std::fabs(tB.position.y) > 1e-3F) ||
      (std::fabs(tB.position.z) > 1e-3F) ||
      (std::fabs(tB.position.x - 2.0F) > 1e-3F)) {
    std::printf("FAIL slider_rail: pos=(%.5f,%.5f,%.5f)\n",
                static_cast<double>(tB.position.x),
                static_cast<double>(tB.position.y),
                static_cast<double>(tB.position.z));
    return 5;
  }
  if (quat_alignment(tB.rotation, math::Quat()) < 1.0F - 1e-5F) {
    return 6;
  }

  physics::set_joint_limits(*world, jid, 0.5F, 1.5F);
  for (int i = 0; i < 120; ++i) {
    if (!step_world(*world, bodies, 2)) {
      return 7;
    }
  }
  world->get_transform(bodies[1], &tB);
  if (std::fabs(tB.position.x - 1.5F) > 1e-3F) {
    std::printf("FAIL slider_limit: x=%.5f expected 1.5\n",
                static_cast<double>(tB.position.x));
    return 8;
  }
  return 0;
}

// ---- Distance: exact length under persistent gravity load ------------------

int test_distance_joint_holds_length_under_gravity() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 1;
  }
  engine::runtime::set_gravity(*world, 0.0F, -10.0F, 0.0F);

  Entity bodies[2] = {
      make_free_body(*world, math::Vec3(0.0F, 10.0F, 0.0F), math::Quat(),
                     0.0F, 0.0F),
      make_free_body(*world, math::Vec3(0.0F, 7.0F, 0.0F), math::Quat(),
                     1.0F, 1.0F)};

  const physics::JointId jid =
      engine::runtime::add_distance_joint(*world, bodies[0], bodies[1], 3.0F);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  for (int i = 0; i < 300; ++i) {
    if (!step_world(*world, bodies, 2)) {
      return 3;
    }
  }

  Transform tA{};
  Transform tB{};
  world->get_transform(bodies[0], &tA);
  world->get_transform(bodies[1], &tB);
  const float dist = math::length(math::sub(tB.position, tA.position));
  if (std::fabs(dist - 3.0F) > 1e-4F) {
    std::printf("FAIL distance_gravity: dist=%.6f expected 3.0\n",
                static_cast<double>(dist));
    return 4;
  }

  const RigidBody *rb = world->get_rigid_body_ptr(bodies[1]);
  if ((rb == nullptr) || (math::length(rb->velocity) > 1e-3F)) {
    return 5;
  }
  return 0;
}

// ---- Articulated chain: two-link pendulum settles to the analytic rest -----

int test_two_link_pendulum_settles_to_rest() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 1;
  }
  engine::runtime::set_gravity(*world, 0.0F, -10.0F, 0.0F);

  Entity bodies[3] = {
      make_free_body(*world, math::Vec3(0.0F, 3.0F, 0.0F), math::Quat(),
                     0.0F, 0.0F),
      make_free_body(*world, math::Vec3(0.0F, 2.0F, 0.0F), math::Quat(),
                     1.0F, 1.0F),
      make_free_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), math::Quat(),
                     1.0F, 1.0F)};

  if ((physics::add_ball_socket_joint(*world, bodies[0], bodies[1],
                                      math::Vec3(0.0F, 3.0F, 0.0F)) ==
       physics::kInvalidJointId) ||
      (physics::add_ball_socket_joint(*world, bodies[1], bodies[2],
                                      math::Vec3(0.0F, 1.0F, 0.0F)) ==
       physics::kInvalidJointId)) {
    return 2;
  }

  {
    RigidBody *rb = world->get_rigid_body_ptr(bodies[2]);
    if (rb == nullptr) {
      return 3;
    }
    rb->velocity = math::Vec3(0.75F, 0.0F, 0.0F);
  }

  for (int i = 0; i < 7200; ++i) {
    if (!step_world(*world, bodies, 3)) {
      return 4;
    }
    if (i == 30) {
      Transform t1{};
      Transform t2{};
      world->get_transform(bodies[1], &t1);
      world->get_transform(bodies[2], &t2);
      const math::Vec3 upperAnchor = math::add(
          t1.position,
          math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), t1.rotation));
      const math::Vec3 lowerAnchorA = math::add(
          t1.position,
          math::rotate_vector(math::Vec3(0.0F, -1.0F, 0.0F), t1.rotation));
      const math::Vec3 lowerAnchorB = math::add(
          t2.position,
          math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), t2.rotation));
      if ((math::length(math::sub(upperAnchor,
                                  math::Vec3(0.0F, 3.0F, 0.0F))) > 2e-3F) ||
          (math::length(math::sub(lowerAnchorA, lowerAnchorB)) > 2e-3F)) {
        std::printf("FAIL pendulum: mid-swing joint separation\n");
        return 5;
      }
    }
  }

  Transform t1{};
  Transform t2{};
  world->get_transform(bodies[1], &t1);
  world->get_transform(bodies[2], &t2);
  const float err1 =
      math::length(math::sub(t1.position, math::Vec3(0.0F, 2.0F, 0.0F)));
  const float err2 = math::length(t2.position);
  if ((err1 > 1e-2F) || (err2 > 1e-2F)) {
    std::printf("FAIL pendulum: rest err1=%.5f err2=%.5f\n",
                static_cast<double>(err1), static_cast<double>(err2));
    return 6;
  }

  const RigidBody *rb1 = world->get_rigid_body_ptr(bodies[1]);
  const RigidBody *rb2 = world->get_rigid_body_ptr(bodies[2]);
  if ((rb1 == nullptr) || (rb2 == nullptr) ||
      (math::length(rb1->velocity) > 1e-2F) ||
      (math::length(rb2->velocity) > 1e-2F)) {
    if ((rb1 != nullptr) && (rb2 != nullptr)) {
      std::printf("FAIL pendulum: residual v1=%.5f v2=%.5f\n",
                  static_cast<double>(math::length(rb1->velocity)),
                  static_cast<double>(math::length(rb2->velocity)));
    }
    return 7;
  }
  return 0;
}

// ---- Static endpoints never rotate from joint torques ----------------------

int test_static_anchor_ignores_default_inertia() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 1;
  }
  engine::runtime::set_gravity(*world, 0.0F, -10.0F, 0.0F);

  Entity bodies[2] = {
      make_free_body(*world, math::Vec3(0.0F, 2.0F, 0.0F), math::Quat(),
                     0.0F, 1.0F),
      make_free_body(*world, math::Vec3(1.0F, 2.0F, 0.0F), math::Quat(),
                     1.0F, 1.0F)};

  if (physics::add_ball_socket_joint(*world, bodies[0], bodies[1],
                                     math::Vec3(0.5F, 2.0F, 0.0F)) ==
      physics::kInvalidJointId) {
    return 2;
  }

  for (int i = 0; i < 120; ++i) {
    if (!step_world(*world, bodies, 2)) {
      return 3;
    }
  }

  Transform tA{};
  world->get_transform(bodies[0], &tA);
  if ((quat_alignment(tA.rotation, math::Quat()) < 1.0F) ||
      (math::length(math::sub(tA.position,
                              math::Vec3(0.0F, 2.0F, 0.0F))) > 0.0F)) {
    std::printf("FAIL static_anchor: moved or rotated\n");
    return 4;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  struct TestCase {
    const char *name;
    int (*func)();
  };

  const TestCase tests[] = {
      {"hinge_limit_clamps_twist_in_radians",
       test_hinge_limit_clamps_twist_in_radians},
      {"hinge_realigns_axes_after_disturbance",
       test_hinge_realigns_axes_after_disturbance},
      {"ball_socket_pendulum_rests_below_pivot",
       test_ball_socket_pendulum_rests_below_pivot},
      {"fixed_joint_preserves_creation_pose",
       test_fixed_joint_preserves_creation_pose},
      {"slider_rail_lock_and_travel_limits",
       test_slider_rail_lock_and_travel_limits},
      {"distance_joint_holds_length_under_gravity",
       test_distance_joint_holds_length_under_gravity},
      {"two_link_pendulum_settles_to_rest",
       test_two_link_pendulum_settles_to_rest},
      {"static_anchor_ignores_default_inertia",
       test_static_anchor_ignores_default_inertia},
  };

  int failures = 0;
  for (const auto &tc : tests) {
    const int result = tc.func();
    if (result != 0) {
      std::printf("FAIL %s (code %d)\n", tc.name, result);
      ++failures;
    } else {
      std::printf("PASS %s\n", tc.name);
    }
  }

  if (failures > 0) {
    std::printf("%d test(s) failed\n", failures);
    return 1;
  }
  std::printf("All joint constraint tests passed\n");
  return 0;
}
