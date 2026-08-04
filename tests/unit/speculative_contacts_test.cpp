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
  auto world = std::unique_ptr<engine::runtime::World>(
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
  auto world = std::unique_ptr<engine::runtime::World>(
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
  auto world = std::unique_ptr<engine::runtime::World>(
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

/// Runs this executable or test program.
/// H-07 regression: speculative pair discovery must not depend on dense
/// index order. The mover (240 m/s, cell 0) and the static target
/// (cell 1) share no unexpanded grid cell — only the mover's velocity
/// expansion bridges them — so the pre-fix scan (which expanded only the
/// insert pass) found the pair solely when the mover held the LARGER
/// index. Both insertion orders must now produce the same speculative
/// impulse: gap 3.0 m at dt=1/60 allows 180 m/s of approach, so the
/// mover's 240 m/s is cut to ~180. Tolerance 0.01 covers the two float
/// roundings (gap/dt division and the impulse multiply) at 1e-5 relative
/// scale; the two orderings run identical arithmetic on one pair, so
/// cross-world equality is asserted exactly.
static void test_speculative_pair_order_independent() noexcept {
  float velocities[2] = {0.0F, 0.0F};

  for (int order = 0; order < 2; ++order) {
    auto world = std::unique_ptr<engine::runtime::World>(
        new (std::nothrow) engine::runtime::World());
    if (world == nullptr) {
      check(false, "Order-independence world allocation");
      return;
    }
    world->end_frame_phase();
    engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

    engine::runtime::Entity mover{};
    engine::runtime::Entity target{};
    auto make_mover = [&world, &mover]() noexcept {
      mover = world->create_entity();
      engine::runtime::Transform t{};
      t.position = engine::math::Vec3(1.0F, 0.0F, 0.0F);
      world->add_transform(mover, t);
      engine::runtime::Collider col{};
      col.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);
      world->add_collider(mover, col);
      engine::runtime::RigidBody rb{};
      rb.inverseMass = 1.0F;
      rb.velocity = engine::math::Vec3(240.0F, 0.0F, 0.0F);
      world->add_rigid_body(mover, rb);
    };
    auto make_target = [&world, &target]() noexcept {
      target = world->create_entity();
      engine::runtime::Transform t{};
      t.position = engine::math::Vec3(4.5F, 0.0F, 0.0F);
      world->add_transform(target, t);
      engine::runtime::Collider col{};
      col.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);
      world->add_collider(target, col);
    };

    if (order == 0) {
      make_mover();
      make_target();
    } else {
      make_target();
      make_mover();
    }

    world->begin_update_phase();
    engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();

    const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(mover);
    if (body == nullptr) {
      check(false, "Order-independence mover body");
      return;
    }
    velocities[order] = body->velocity.x;
  }

  check(std::fabs(velocities[0] - 180.0F) <= 0.01F,
        "Speculative impulse applies with mover at the smaller index");
  check(std::fabs(velocities[1] - 180.0F) <= 0.01F,
        "Speculative impulse applies with mover at the larger index");
  check(velocities[0] == velocities[1],
        "Speculative impulse identical for both insertion orders");
}

int main() {
  std::printf("=== Speculative Contacts Tests (P1-M3-E2) ===\n");

  test_ball_approaching_wall_no_penetration();
  test_speculative_no_ghost_collision();
  test_speculative_approaching_spheres();
  test_speculative_pair_order_independent();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
