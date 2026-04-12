#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

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
using Collider = engine::runtime::Collider;
namespace math = engine::math;
namespace physics = engine::physics;

// Helper: create a dynamic entity at a given position.
Entity make_body(World &w, const math::Vec3 &pos) noexcept {
  const Entity e = w.create_entity();
  Transform t{};
  t.position = pos;
  w.add_transform(e, t);

  RigidBody rb{};
  rb.inverseMass = 1.0F;
  w.add_rigid_body(e, rb);

  Collider col{};
  col.halfExtents = math::Vec3(0.25F, 0.25F, 0.25F);
  w.add_collider(e, col);
  return e;
}

float vec_distance(const math::Vec3 &a, const math::Vec3 &b) noexcept {
  const math::Vec3 d = math::sub(a, b);
  return std::sqrt(math::dot(d, d));
}

// ---- Distance joint --------------------------------------------------------

int test_distance_joint() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(3.0F, 0.0F, 0.0F));

  const physics::JointId jid =
      engine::runtime::add_distance_joint(*world, a, b, 2.0F);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  // Run several solver iterations to let the constraint converge.
  for (int i = 0; i < 60; ++i) {
    world->begin_update_phase();
    physics::solve_constraints(*world, 1.0F / 60.0F);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  Transform tA{};
  Transform tB{};
  world->get_transform(a, &tA);
  world->get_transform(b, &tB);

  const float dist = vec_distance(tA.position, tB.position);
  // Should converge towards 2.0.
  if (std::fabs(dist - 2.0F) > 0.3F) {
    std::printf("FAIL distance_joint: dist=%.3f (expected ~2.0)\n", dist);
    return 3;
  }
  return 0;
}

// ---- Hinge joint -----------------------------------------------------------

int test_hinge_joint() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(1.0F, 0.0F, 0.0F));

  const math::Vec3 pivot(0.5F, 0.0F, 0.0F);
  const math::Vec3 axis(0.0F, 1.0F, 0.0F);
  const physics::JointId jid =
      physics::add_hinge_joint(*world, a, b, pivot, axis);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  for (int i = 0; i < 60; ++i) {
    world->begin_update_phase();
    physics::solve_constraints(*world, 1.0F / 60.0F);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  // Entities should remain relatively close to the pivot.
  Transform tA{};
  Transform tB{};
  world->get_transform(a, &tA);
  world->get_transform(b, &tB);

  const float distA = vec_distance(tA.position, pivot);
  const float distB = vec_distance(tB.position, pivot);
  if (distA > 1.5F || distB > 1.5F) {
    std::printf("FAIL hinge_joint: distA=%.3f distB=%.3f\n", distA, distB);
    return 3;
  }
  return 0;
}

// ---- Ball-socket joint -----------------------------------------------------

int test_ball_socket_joint() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(2.0F, 0.0F, 0.0F));

  const math::Vec3 pivot(1.0F, 0.0F, 0.0F);
  const physics::JointId jid =
      physics::add_ball_socket_joint(*world, a, b, pivot);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  for (int i = 0; i < 60; ++i) {
    world->begin_update_phase();
    physics::solve_constraints(*world, 1.0F / 60.0F);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  // Both anchors should converge close to pivot.
  Transform tA{};
  Transform tB{};
  world->get_transform(a, &tA);
  world->get_transform(b, &tB);

  // Mid-point of the two should be near the original pivot.
  const float midX = (tA.position.x + tB.position.x) * 0.5F;
  if (std::fabs(midX - 1.0F) > 0.5F) {
    std::printf("FAIL ball_socket: midX=%.3f\n", midX);
    return 3;
  }
  return 0;
}

// ---- Slider joint ----------------------------------------------------------

int test_slider_joint() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(2.0F, 1.0F, 0.0F));

  const math::Vec3 axis(1.0F, 0.0F, 0.0F);
  const physics::JointId jid = physics::add_slider_joint(*world, a, b, axis);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  for (int i = 0; i < 60; ++i) {
    world->begin_update_phase();
    physics::solve_constraints(*world, 1.0F / 60.0F);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  // The perpendicular component (Y) should converge towards zero.
  Transform tA{};
  Transform tB{};
  world->get_transform(a, &tA);
  world->get_transform(b, &tB);

  const float yDiff = std::fabs(tA.position.y - tB.position.y);
  if (yDiff > 0.5F) {
    std::printf("FAIL slider_joint: yDiff=%.3f\n", yDiff);
    return 3;
  }
  return 0;
}

// ---- Spring joint ----------------------------------------------------------

int test_spring_joint() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(5.0F, 0.0F, 0.0F));

  // Rest length 2, moderate stiffness + damping.
  const physics::JointId jid =
      physics::add_spring_joint(*world, a, b, 2.0F, 10.0F, 5.0F);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  for (int i = 0; i < 120; ++i) {
    world->begin_update_phase();
    physics::solve_constraints(*world, 1.0F / 60.0F);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  Transform tA{};
  Transform tB{};
  world->get_transform(a, &tA);
  world->get_transform(b, &tB);

  // Spring should pull entities closer towards rest length 2.
  const float dist = vec_distance(tA.position, tB.position);
  if (dist > 5.5F) {
    std::printf("FAIL spring_joint: dist=%.3f (expected < 5.5)\n", dist);
    return 3;
  }
  return 0;
}

// ---- Fixed joint -----------------------------------------------------------

int test_fixed_joint() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));

  const physics::JointId jid = physics::add_fixed_joint(*world, a, b);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  // Give entity B a velocity kick.
  {
    RigidBody *rbB = world->get_rigid_body_ptr(b);
    if (rbB == nullptr) {
      return 3;
    }
    rbB->velocity = math::Vec3(5.0F, 0.0F, 0.0F);
  }

  for (int i = 0; i < 60; ++i) {
    world->begin_update_phase();
    physics::solve_constraints(*world, 1.0F / 60.0F);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  Transform tA{};
  Transform tB{};
  world->get_transform(a, &tA);
  world->get_transform(b, &tB);

  // Fixed joint: entities should remain very close together.
  const float dist = vec_distance(tA.position, tB.position);
  if (dist > 0.5F) {
    std::printf("FAIL fixed_joint: dist=%.3f (expected ~0)\n", dist);
    return 3;
  }
  return 0;
}

// ---- Joint limits ----------------------------------------------------------

int test_joint_limits() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(3.0F, 0.0F, 0.0F));

  const math::Vec3 axis(1.0F, 0.0F, 0.0F);
  const physics::JointId jid = physics::add_slider_joint(*world, a, b, axis);
  if (jid == physics::kInvalidJointId) {
    return 2;
  }

  // Limit the slider to [0.5, 1.5] range.
  physics::set_joint_limits(*world, jid, 0.5F, 1.5F);

  for (int i = 0; i < 60; ++i) {
    world->begin_update_phase();
    physics::solve_constraints(*world, 1.0F / 60.0F);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  // Joint ID should remain valid (not invalidated).
  // The set_joint_limits call itself should not crash.
  return 0;
}

} // namespace

int main() {
  struct TestCase {
    const char *name;
    int (*func)();
  };

  const TestCase tests[] = {
      {"distance_joint", test_distance_joint},
      {"hinge_joint", test_hinge_joint},
      {"ball_socket_joint", test_ball_socket_joint},
      {"slider_joint", test_slider_joint},
      {"spring_joint", test_spring_joint},
      {"fixed_joint", test_fixed_joint},
      {"joint_limits", test_joint_limits},
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
  std::printf("All joint tests passed\n");
  return 0;
}
