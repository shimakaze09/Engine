// Regression tests for issue #98: CCD impact response must consume the
// pair's combined material restitution and inverse-mass split instead of
// hard-coding a perfectly elastic world-space reflection.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
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

// Result of one CCD bullet-vs-wall step with given material restitutions.
struct BulletResult {
  engine::math::Vec3 velocity{};
  float positionX = 0.0F;
  bool valid = false;
};

// Fires a 200 m/s bullet at a static thin wall and runs one production
// step (integrate + CCD, resolve, commit); vz carries tangential motion.
static BulletResult run_bullet(float bulletRestitution, float wallRestitution,
                               float tangentialZ) noexcept {
  BulletResult result{};
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return result;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto bullet = world->create_entity();
  engine::runtime::Transform bulletT{};
  world->add_transform(bullet, bulletT);
  engine::runtime::Collider bulletCol{};
  bulletCol.shape = engine::runtime::ColliderShape::Sphere;
  bulletCol.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  bulletCol.restitution = bulletRestitution;
  world->add_collider(bullet, bulletCol);
  engine::runtime::RigidBody bulletRB{};
  bulletRB.inverseMass = 1.0F;
  bulletRB.velocity = engine::math::Vec3(200.0F, 0.0F, tangentialZ);
  world->add_rigid_body(bullet, bulletRB);

  const auto wall = world->create_entity();
  engine::runtime::Transform wallT{};
  wallT.position = engine::math::Vec3(2.0F, 0.0F, 0.0F);
  world->add_transform(wall, wallT);
  engine::runtime::Collider wallCol{};
  wallCol.halfExtents = engine::math::Vec3(0.05F, 2.0F, 2.0F);
  wallCol.restitution = wallRestitution;
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
  engine::runtime::Transform after{};
  if ((body == nullptr) || !world->get_transform(bullet, &after)) {
    return result;
  }
  result.velocity = body->velocity;
  result.positionX = after.position.x;
  result.valid = true;
  return result;
}

/// Restitution 0 on both materials removes the incoming normal velocity
/// without any rebound (the impulse is a single scalar along the exact
/// axis-aligned face normal, so 1e-3 absolute covers float rounding).
static void test_restitution_zero_dead_stop() noexcept {
  const BulletResult r = run_bullet(0.0F, 0.0F, 0.0F);
  check(r.valid, "Dead-stop world ran");
  check(r.valid && (std::fabs(r.velocity.x) <= 1.0e-3F),
        "Restitution 0 stops the bullet instead of full-speed rebound");
  check(r.valid && (r.positionX < 2.0F), "Restitution 0 bullet does not tunnel");
}

/// Restitution 1 preserves the full rebound speed (the pre-fix contract,
/// kept as the elastic boundary).
static void test_restitution_one_full_bounce() noexcept {
  const BulletResult r = run_bullet(1.0F, 1.0F, 0.0F);
  check(r.valid && (std::fabs(r.velocity.x + 200.0F) <= 0.2F),
        "Restitution 1 rebounds at full speed");
}

/// Restitution 0.5 rebounds at half the approach speed.
static void test_restitution_half_bounce() noexcept {
  const BulletResult r = run_bullet(0.5F, 0.5F, 0.0F);
  check(r.valid && (std::fabs(r.velocity.x + 100.0F) <= 0.1F),
        "Restitution 0.5 rebounds at half speed");
}

/// Mixed materials combine with the same max rule as the discrete solver.
static void test_mixed_materials_use_max() noexcept {
  const BulletResult r = run_bullet(0.0F, 0.5F, 0.0F);
  check(r.valid && (std::fabs(r.velocity.x + 100.0F) <= 0.1F),
        "Mixed materials rebound with max(restitution)");
}

/// The CCD impulse acts along the contact normal only, so tangential
/// velocity survives the impact up to EPA normal noise (observed ~1e-6 on
/// an axis face) scaled by the 200 m/s impulse; 1e-3 bounds that without
/// admitting any friction-like response.
static void test_tangential_velocity_preserved() noexcept {
  const BulletResult r = run_bullet(0.0F, 0.0F, 4.0F);
  check(r.valid && (std::fabs(r.velocity.z - 4.0F) <= 1.0e-3F),
        "Tangential velocity unchanged through CCD impact");
}

/// A stationary dynamic target cannot apply its own sweep share, so CCD
/// only clamps the mover to a safe TOI and the discrete solver exchanges
/// the momentum: after a few steps both bodies move forward, linear
/// momentum is conserved, and nothing tunnels.
static void test_heavy_dynamic_target_shares_impulse() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto bullet = world->create_entity();
  engine::runtime::Transform bulletT{};
  world->add_transform(bullet, bulletT);
  engine::runtime::Collider bulletCol{};
  bulletCol.shape = engine::runtime::ColliderShape::Sphere;
  bulletCol.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  bulletCol.restitution = 0.0F;
  world->add_collider(bullet, bulletCol);
  engine::runtime::RigidBody bulletRB{};
  bulletRB.inverseMass = 1.0F;
  bulletRB.velocity = engine::math::Vec3(200.0F, 0.0F, 0.0F);
  world->add_rigid_body(bullet, bulletRB);

  const auto block = world->create_entity();
  engine::runtime::Transform blockT{};
  blockT.position = engine::math::Vec3(2.0F, 0.0F, 0.0F);
  world->add_transform(block, blockT);
  engine::runtime::Collider blockCol{};
  blockCol.halfExtents = engine::math::Vec3(0.5F, 2.0F, 2.0F);
  blockCol.restitution = 0.0F;
  world->add_collider(block, blockCol);
  engine::runtime::RigidBody blockRB{};
  blockRB.inverseMass = 0.5F;
  world->add_rigid_body(block, blockRB);

  for (int i = 0; i < 3; ++i) {
    world->begin_update_phase();
    engine::runtime::step_physics(*world, 1.0F / 60.0F);
    engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(bullet);
  const engine::runtime::RigidBody *target = world->get_rigid_body_ptr(block);
  engine::runtime::Transform after{};
  engine::runtime::Transform blockAfter{};
  if ((body == nullptr) || (target == nullptr) ||
      !world->get_transform(bullet, &after) ||
      !world->get_transform(block, &blockAfter)) {
    check(false, "Body lookup failed");
    return;
  }
  // Mass 1 at 200 m/s vs mass 2 at rest: the impulses stay internal to the
  // pair, so px = 1*vBullet + 2*vBlock must remain 200 within float noise.
  const float momentum = body->velocity.x + 2.0F * target->velocity.x;
  check(std::fabs(momentum - 200.0F) <= 0.1F,
        "Discrete exchange conserves pair momentum after CCD clamp");
  check(target->velocity.x > 1.0F, "Heavy target receives forward momentum");
  check((body->velocity.x > 0.0F) && (body->velocity.x < 200.0F),
        "Mover slows without reversing against the heavy target");
  check(after.position.x < blockAfter.position.x,
        "Mover stays on its side of the heavy target");
}

/// The CCD response is deterministic: identical worlds produce bit-equal
/// post-impact velocities.
static void test_ccd_response_deterministic() noexcept {
  const BulletResult a = run_bullet(0.5F, 0.25F, 2.0F);
  const BulletResult b = run_bullet(0.5F, 0.25F, 2.0F);
  check(a.valid && b.valid && (a.velocity.x == b.velocity.x) &&
            (a.velocity.y == b.velocity.y) && (a.velocity.z == b.velocity.z) &&
            (a.positionX == b.positionX),
        "CCD response is bit-deterministic across identical runs");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== CCD Material Response Tests (issue #98) ===\n");

  test_restitution_zero_dead_stop();
  test_restitution_one_full_bounce();
  test_restitution_half_bounce();
  test_mixed_materials_use_max();
  test_tangential_velocity_preserved();
  test_heavy_dynamic_target_shares_impulse();
  test_ccd_response_deterministic();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
