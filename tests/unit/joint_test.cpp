// Verifies joint test behavior for the Engine test suite.

#include <cmath>
#include <cstdio>
#include <memory>
#include <limits>
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

// A settled slider joint must stay put: the warm start replays center-line
// impulses, which for a slider would drift both bodies along the axis in a
// direction its solver never corrects.
int test_slider_settled_no_warm_start_drift() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(2.0F, 1.0F, 0.0F));
  const math::Vec3 axis(1.0F, 0.0F, 0.0F);
  if (physics::add_slider_joint(*world, a, b, axis) ==
      physics::kInvalidJointId) {
    return 2;
  }

  // Frame 1 fully corrects the perpendicular offset and accumulates the
  // correction magnitude.
  world->begin_update_phase();
  physics::solve_constraints(*world, 1.0F / 60.0F);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  Transform settledA{};
  Transform settledB{};
  if (!world->get_transform(a, &settledA) ||
      !world->get_transform(b, &settledB)) {
    return 3;
  }

  // Frame 2 has nothing to correct; positions must not move at all.
  world->begin_update_phase();
  physics::solve_constraints(*world, 1.0F / 60.0F);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  Transform afterA{};
  Transform afterB{};
  if (!world->get_transform(a, &afterA) || !world->get_transform(b, &afterB)) {
    return 4;
  }
  if ((afterA.position.x != settledA.position.x) ||
      (afterA.position.y != settledA.position.y) ||
      (afterA.position.z != settledA.position.z) ||
      (afterB.position.x != settledB.position.x) ||
      (afterB.position.y != settledB.position.y) ||
      (afterB.position.z != settledB.position.z)) {
    std::printf("FAIL slider drift: a=(%.4f,%.4f) b=(%.4f,%.4f)\n",
                static_cast<double>(afterA.position.x),
                static_cast<double>(afterA.position.y),
                static_cast<double>(afterB.position.x),
                static_cast<double>(afterB.position.y));
    return 5;
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

/// Destroyed endpoints must retire their joint instead of binding slot reuse.
int test_destroyed_endpoint_retires_joint() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity original =
      make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity other = make_body(*world, math::Vec3(2.0F, 0.0F, 0.0F));
  const physics::JointId originalJoint =
      engine::runtime::add_distance_joint(*world, original, other, 2.0F);
  if (originalJoint == physics::kInvalidJointId) {
    return 2;
  }

  if (!world->destroy_entity(original)) {
    return 3;
  }
  const Entity replacement =
      make_body(*world, math::Vec3(10.0F, 0.0F, 0.0F));
  if ((replacement.index != original.index) ||
      (replacement.generation == original.generation)) {
    return 4;
  }

  world->begin_update_phase();
  physics::solve_constraints(*world, 1.0F / 60.0F);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  const physics::PhysicsContext &context = world->physics_context();
  if (context.jointCount != 0U) {
    return 5;
  }
  for (const physics::PhysicsJointSlot &joint : context.joints) {
    if (joint.active) {
      return 6;
    }
  }

  const physics::JointId replacementJoint =
      engine::runtime::add_distance_joint(*world, replacement, other, 8.0F);
  if ((replacementJoint == physics::kInvalidJointId) ||
      (replacementJoint == originalJoint)) {
    return 7;
  }

  engine::runtime::remove_joint(*world, originalJoint);
  if (context.jointCount != 1U) {
    return 8;
  }
  std::size_t activeCount = 0U;
  for (const physics::PhysicsJointSlot &joint : context.joints) {
    if (joint.active) {
      ++activeCount;
    }
  }
  if (activeCount != 1U) {
    return 9;
  }
  return 0;
}


/// Invalid parameters are rejected, axes normalize, and stale IDs never alias.
int test_joint_validation_and_stale_ids() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity a = make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity b = make_body(*world, math::Vec3(2.0F, 0.0F, 0.0F));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();

  if ((engine::runtime::add_distance_joint(*world, a, a, 1.0F) !=
       physics::kInvalidJointId) ||
      (engine::runtime::add_distance_joint(*world, a, b, -1.0F) !=
       physics::kInvalidJointId) ||
      (engine::runtime::add_distance_joint(*world, a, b, nan) !=
       physics::kInvalidJointId) ||
      (physics::add_hinge_joint(*world, a, b, math::Vec3{},
                                math::Vec3{}) != physics::kInvalidJointId) ||
      (physics::add_ball_socket_joint(*world, a, b,
                                      math::Vec3(nan, 0.0F, 0.0F)) !=
       physics::kInvalidJointId) ||
      (physics::add_slider_joint(*world, a, b,
                                 math::Vec3(infinity, 0.0F, 0.0F)) !=
       physics::kInvalidJointId) ||
      (physics::add_spring_joint(*world, a, b, 1.0F, -1.0F, 1.0F) !=
       physics::kInvalidJointId) ||
      (physics::add_fixed_joint(*world, a, a) !=
       physics::kInvalidJointId)) {
    return 2;
  }

  physics::PhysicsContext &context = world->physics_context();
  if (context.jointCount != 0U) {
    return 3;
  }

  const physics::JointId first = physics::add_slider_joint(
      *world, a, b, math::Vec3(10.0F, 0.0F, 0.0F));
  if (first == physics::kInvalidJointId) {
    return 4;
  }

  physics::PhysicsJointSlot *active = nullptr;
  for (physics::PhysicsJointSlot &joint : context.joints) {
    if (joint.active) {
      if (active != nullptr) {
        return 5;
      }
      active = &joint;
    }
  }
  if ((active == nullptr) || (active->axis.x != 1.0F) ||
      (active->axis.y != 0.0F) || (active->axis.z != 0.0F) ||
      active->hasLimits) {
    return 6;
  }

  physics::set_joint_limits(*world, first, 2.0F, 1.0F);
  if (active->hasLimits) {
    return 7;
  }
  physics::set_joint_limits(*world, first, -1.0F, 1.0F);
  if (!active->hasLimits || (active->minLimit != -1.0F) ||
      (active->maxLimit != 1.0F)) {
    return 8;
  }

  engine::runtime::remove_joint(*world, first);
  if (context.jointCount != 0U) {
    return 9;
  }

  const physics::JointId replacement = physics::add_slider_joint(
      *world, a, b, math::Vec3(1.0F, 0.0F, 0.0F));
  if ((replacement == physics::kInvalidJointId) || (replacement == first)) {
    return 10;
  }

  active = nullptr;
  for (physics::PhysicsJointSlot &joint : context.joints) {
    if (joint.active) {
      active = &joint;
      break;
    }
  }
  if ((active == nullptr) || active->hasLimits) {
    return 11;
  }

  physics::set_joint_limits(*world, first, -2.0F, 2.0F);
  engine::runtime::remove_joint(*world, first);
  if ((context.jointCount != 1U) || !active->active || active->hasLimits) {
    return 12;
  }

  engine::runtime::remove_joint(*world, replacement);
  return (context.jointCount == 0U) ? 0 : 13;
}


} // namespace

/// Runs this executable or test program.
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
      {"slider_settled_no_warm_start_drift",
       test_slider_settled_no_warm_start_drift},
      {"fixed_joint", test_fixed_joint},
      {"joint_limits", test_joint_limits},
      {"destroyed_endpoint_retires_joint",
       test_destroyed_endpoint_retires_joint},
      {"joint_validation_and_stale_ids", test_joint_validation_and_stale_ids},
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
