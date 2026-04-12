// Speculative contacts tests — P1-M3-E2c.
// Verifies that speculative contact generation prevents visible penetration
// and does not cause ghost collisions.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

static int g_passed = 0;
static int g_failed = 0;

static void check(bool condition, const char *name) noexcept {
  if (condition) {
    ++g_passed;
    std::printf("  PASS: %s\n", name);
  } else {
    ++g_failed;
    std::printf("  FAIL: %s\n", name);
  }
}

static void test_ball_approaching_wall_no_penetration() noexcept {
  // A ball rolling toward a wall at moderate speed should stop without
  // any visible penetration frame.
  auto world =
      std::unique_ptr<engine::runtime::World>(
          new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation");
    return;
  }
  world->end_frame_phase();

  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto ball = world->create_entity();
  const auto wall = world->create_entity();

  // Ball: sphere at x=-0.6, moving at +10 m/s toward wall.
  // Wall: AABB at x=0.0 with halfExtent.x = 0.5.
  // Ball right edge = -0.6 + 0.3 = -0.3. Wall left edge = -0.5.
  // Gap = 0.2m. At 10 m/s, 1/60s = 0.167m travel.
  engine::runtime::Transform ballT{};
  ballT.position = engine::math::Vec3(-0.6F, 0.0F, 0.0F);
  world->add_transform(ball, ballT);

  engine::runtime::Collider ballCol{};
  ballCol.shape = engine::runtime::ColliderShape::Sphere;
  ballCol.halfExtents = engine::math::Vec3(0.3F, 0.3F, 0.3F);
  world->add_collider(ball, ballCol);

  engine::runtime::RigidBody ballRB{};
  ballRB.inverseMass = 1.0F;
  ballRB.velocity = engine::math::Vec3(10.0F, 0.0F, 0.0F);
  world->add_rigid_body(ball, ballRB);

  engine::runtime::Transform wallT{};
  wallT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  world->add_transform(wall, wallT);

  engine::runtime::Collider wallCol{};
  wallCol.shape = engine::runtime::ColliderShape::AABB;
  wallCol.halfExtents = engine::math::Vec3(0.5F, 2.0F, 2.0F);
  world->add_collider(wall, wallCol);

  engine::runtime::RigidBody wallRB{};
  wallRB.inverseMass = 0.0F;
  world->add_rigid_body(wall, wallRB);

  // Run multiple physics frames.
  for (int frame = 0; frame < 10; ++frame) {
    world->begin_update_phase();
    engine::runtime::step_physics(*world, 1.0F / 60.0F);
    engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  engine::runtime::Transform finalBall{};
  world->get_transform(ball, &finalBall);

  // Ball right edge = center.x + 0.3, wall left edge = -0.5.
  const float ballRightEdge = finalBall.position.x + 0.3F;
  const float wallLeftEdge = -0.5F;
  check(ballRightEdge < wallLeftEdge + 0.15F,
        "Ball stops at wall without deep penetration");
}

static void test_speculative_no_ghost_collision() noexcept {
  // Two objects moving parallel (not toward each other) should NOT get
  // ghost collision from speculative contacts.
  auto world =
      std::unique_ptr<engine::runtime::World>(
          new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation");
    return;
  }
  world->end_frame_phase();

  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto entA = world->create_entity();
  const auto entB = world->create_entity();

  // Object A: moving along +Y at x=0.
  engine::runtime::Transform tA{};
  tA.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  world->add_transform(entA, tA);

  engine::runtime::Collider colA{};
  colA.shape = engine::runtime::ColliderShape::AABB;
  colA.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(entA, colA);

  engine::runtime::RigidBody rbA{};
  rbA.inverseMass = 1.0F;
  rbA.velocity = engine::math::Vec3(0.0F, 10.0F, 0.0F);
  world->add_rigid_body(entA, rbA);

  // Object B: also moving along +Y, separated on x-axis.
  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(2.0F, 0.0F, 0.0F);
  world->add_transform(entB, tB);

  engine::runtime::Collider colB{};
  colB.shape = engine::runtime::ColliderShape::AABB;
  colB.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(entB, colB);

  engine::runtime::RigidBody rbB{};
  rbB.inverseMass = 1.0F;
  rbB.velocity = engine::math::Vec3(0.0F, 10.0F, 0.0F);
  world->add_rigid_body(entB, rbB);

  world->begin_update_phase();
  engine::runtime::step_physics(*world, 1.0F / 60.0F);
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  const engine::runtime::RigidBody *bodyA = world->get_rigid_body_ptr(entA);
  const engine::runtime::RigidBody *bodyB = world->get_rigid_body_ptr(entB);

  check(bodyA != nullptr && bodyA->velocity.y > 9.0F,
        "Object A keeps Y velocity (no ghost collision)");
  check(bodyB != nullptr && bodyB->velocity.y > 9.0F,
        "Object B keeps Y velocity (no ghost collision)");

  // X velocity should remain near zero.
  check(bodyA != nullptr && std::fabs(bodyA->velocity.x) < 0.5F,
        "Object A has no spurious X velocity");
  check(bodyB != nullptr && std::fabs(bodyB->velocity.x) < 0.5F,
        "Object B has no spurious X velocity");
}

static void test_speculative_approaching_spheres() noexcept {
  // Two spheres approaching each other — speculative contacts should
  // prevent deep penetration.
  auto world =
      std::unique_ptr<engine::runtime::World>(
          new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation");
    return;
  }
  world->end_frame_phase();

  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto entA = world->create_entity();
  const auto entB = world->create_entity();

  engine::runtime::Transform tA{};
  tA.position = engine::math::Vec3(-1.0F, 0.0F, 0.0F);
  world->add_transform(entA, tA);

  engine::runtime::Collider colA{};
  colA.shape = engine::runtime::ColliderShape::Sphere;
  colA.halfExtents = engine::math::Vec3(0.4F, 0.4F, 0.4F);
  world->add_collider(entA, colA);

  engine::runtime::RigidBody rbA{};
  rbA.inverseMass = 1.0F;
  rbA.velocity = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  world->add_rigid_body(entA, rbA);

  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  world->add_transform(entB, tB);

  engine::runtime::Collider colB{};
  colB.shape = engine::runtime::ColliderShape::Sphere;
  colB.halfExtents = engine::math::Vec3(0.4F, 0.4F, 0.4F);
  world->add_collider(entB, colB);

  engine::runtime::RigidBody rbB{};
  rbB.inverseMass = 1.0F;
  rbB.velocity = engine::math::Vec3(-5.0F, 0.0F, 0.0F);
  world->add_rigid_body(entB, rbB);

  // Run a few frames.
  for (int frame = 0; frame < 5; ++frame) {
    world->begin_update_phase();
    engine::runtime::step_physics(*world, 1.0F / 60.0F);
    engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  engine::runtime::Transform finalA{};
  engine::runtime::Transform finalB{};
  world->get_transform(entA, &finalA);
  world->get_transform(entB, &finalB);
  const float dist = std::fabs(finalB.position.x - finalA.position.x);
  const float sumR = 0.4F + 0.4F;
  check(dist >= sumR - 0.2F, "Approaching spheres don't deeply penetrate");
}

int main() {
  std::printf("=== Speculative Contacts Tests (P1-M3-E2) ===\n");

  test_ball_approaching_wall_no_penetration();
  test_speculative_no_ghost_collision();
  test_speculative_approaching_spheres();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
