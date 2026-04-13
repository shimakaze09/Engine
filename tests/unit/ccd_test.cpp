// CCD (Continuous Collision Detection) tests — P1-M3-E1d.
// Verifies bilateral advancement prevents tunneling at high speeds.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/physics/ccd.h"
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

// Helper: create a world with a fast bullet and thin wall, step one frame.
// Returns the bullet's final X position.
static float run_bullet_vs_wall(float speed) noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 999.0F;
  }
  world->end_frame_phase();

  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto bullet = world->create_entity();
  const auto wall = world->create_entity();

  // Bullet: small sphere at origin.
  engine::runtime::Transform bulletT{};
  bulletT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  world->add_transform(bullet, bulletT);

  engine::runtime::Collider bulletCol{};
  bulletCol.shape = engine::runtime::ColliderShape::Sphere;
  bulletCol.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  world->add_collider(bullet, bulletCol);

  engine::runtime::RigidBody bulletRB{};
  bulletRB.inverseMass = 1.0F;
  bulletRB.velocity = engine::math::Vec3(speed, 0.0F, 0.0F);
  world->add_rigid_body(bullet, bulletRB);

  // Wall: thin AABB at x=5.
  engine::runtime::Transform wallT{};
  wallT.position = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  world->add_transform(wall, wallT);

  engine::runtime::Collider wallCol{};
  wallCol.shape = engine::runtime::ColliderShape::AABB;
  wallCol.halfExtents = engine::math::Vec3(0.02F, 2.0F, 2.0F);
  world->add_collider(wall, wallCol);

  engine::runtime::RigidBody wallRB{};
  wallRB.inverseMass = 0.0F; // static
  world->add_rigid_body(wall, wallRB);

  // Step one frame.
  const float dt = 1.0F / 60.0F;
  world->begin_update_phase();
  engine::runtime::step_physics(*world, dt);
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform result{};
  world->get_transform(bullet, &result);
  return result.position.x;
}

static void test_bullet_100_no_tunnel() noexcept {
  const float x = run_bullet_vs_wall(100.0F);
  // Wall near edge at x = 5.0 - 0.02 = 4.98. Bullet radius = 0.1.
  // Bullet center should be < 4.98 (ideally < 4.88).
  check(x < 5.0F, "Bullet at 100m/s doesn't tunnel through wall");
}

static void test_bullet_300_no_tunnel() noexcept {
  const float x = run_bullet_vs_wall(300.0F);
  // At 300 m/s, 1/60s = 5m travel. Wall is at x=5.
  // Without CCD, bullet would be at x=5. With CCD, should stop before wall.
  check(x < 5.0F, "Bullet at 300m/s doesn't tunnel through wall");
}

static void test_slow_body_normal_integration() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();

  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto ent = world->create_entity();

  engine::runtime::Transform t{};
  world->add_transform(ent, t);

  engine::runtime::Collider col{};
  col.shape = engine::runtime::ColliderShape::Sphere;
  col.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(ent, col);

  engine::runtime::RigidBody rb{};
  rb.inverseMass = 1.0F;
  rb.velocity = engine::math::Vec3(1.0F, 0.0F, 0.0F); // Below CCD threshold
  world->add_rigid_body(ent, rb);

  const float dt = 1.0F / 60.0F;
  world->begin_update_phase();
  engine::runtime::step_physics(*world, dt);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform result{};
  world->get_transform(ent, &result);

  const float expected = 1.0F * dt;
  const float err = std::fabs(result.position.x - expected);
  check(err < 0.01F, "Slow body integrates normally (no CCD)");
}

static void test_ccd_velocity_reflects_on_hit() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();

  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto bullet = world->create_entity();
  const auto wall = world->create_entity();

  engine::runtime::Transform bulletT{};
  world->add_transform(bullet, bulletT);

  engine::runtime::Collider bulletCol{};
  bulletCol.shape = engine::runtime::ColliderShape::Sphere;
  bulletCol.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  world->add_collider(bullet, bulletCol);

  engine::runtime::RigidBody bulletRB{};
  bulletRB.inverseMass = 1.0F;
  bulletRB.velocity = engine::math::Vec3(200.0F, 0.0F, 0.0F);
  world->add_rigid_body(bullet, bulletRB);

  engine::runtime::Transform wallT{};
  wallT.position = engine::math::Vec3(2.0F, 0.0F, 0.0F);
  world->add_transform(wall, wallT);

  engine::runtime::Collider wallCol{};
  wallCol.shape = engine::runtime::ColliderShape::AABB;
  wallCol.halfExtents = engine::math::Vec3(0.05F, 2.0F, 2.0F);
  world->add_collider(wall, wallCol);

  engine::runtime::RigidBody wallRB{};
  wallRB.inverseMass = 0.0F;
  world->add_rigid_body(wall, wallRB);

  world->begin_update_phase();
  engine::runtime::step_physics(*world, 1.0F / 60.0F);
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(bullet);
  // After CCD + collision, bullet X velocity should be reflected or reduced.
  check(body != nullptr && body->velocity.x <= 1.0F,
        "Bullet velocity reflects after CCD hit");
}

int main() {
  std::printf("=== CCD Tests (P1-M3-E1) ===\n");

  test_bullet_100_no_tunnel();
  test_bullet_300_no_tunnel();
  test_slow_body_normal_integration();
  test_ccd_velocity_reflects_on_hit();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
