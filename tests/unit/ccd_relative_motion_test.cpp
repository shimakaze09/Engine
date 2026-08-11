// Regression tests for issue #106: CCD candidate gating must use relative
// motion, and the first step after world creation must see moving targets
// (primed snapshot) so two fast bodies closing head-on can never tunnel.

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

// Adds a dynamic unit-diameter sphere with the given x position/velocity.
static engine::core::Entity add_fast_sphere(engine::runtime::World &world,
                                            float x, float vx) noexcept {
  const auto entity = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(x, 0.0F, 0.0F);
  world.add_transform(entity, transform);
  engine::runtime::Collider collider{};
  collider.shape = engine::runtime::ColliderShape::Sphere;
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world.add_collider(entity, collider);
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  body.velocity = engine::math::Vec3(vx, 0.0F, 0.0F);
  world.add_rigid_body(entity, body);
  return entity;
}

// Runs one production fixed step: integrate + CCD, resolve, commit.
static void run_step(engine::runtime::World &world) noexcept {
  world.begin_update_phase();
  engine::runtime::step_physics(world, 1.0F / 60.0F);
  engine::runtime::resolve_collisions(world);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
}

/// The issue's exact reproduction with no prior snapshot: spheres at x=0
/// and x=10 closing at +-360 m/s cross completely within one step if the
/// gate ignores target motion. The primed snapshot plus relative gating
/// must stop them on their own sides with a symmetric rebound.
static void test_head_on_first_step_no_snapshot() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto left = add_fast_sphere(*world, 0.0F, 360.0F);
  const auto right = add_fast_sphere(*world, 10.0F, -360.0F);

  run_step(*world);

  engine::runtime::Transform leftT{};
  engine::runtime::Transform rightT{};
  const engine::runtime::RigidBody *leftB = world->get_rigid_body_ptr(left);
  const engine::runtime::RigidBody *rightB = world->get_rigid_body_ptr(right);
  if (!world->get_transform(left, &leftT) ||
      !world->get_transform(right, &rightT) || (leftB == nullptr) ||
      (rightB == nullptr)) {
    check(false, "Body lookup failed");
    return;
  }
  check(leftT.position.x < rightT.position.x,
        "First-step head-on pair does not swap sides");
  check(leftT.position.x < 4.5F, "Left body clamps before the crossing point");
  check(rightT.position.x > 5.5F,
        "Right body clamps before the crossing point");
  check(leftB->velocity.x < 0.0F, "Left body rebounds");
  check(rightB->velocity.x > 0.0F, "Right body rebounds");
  // Symmetric sweeps apply symmetric shares: pair momentum stays zero
  // within EPA normal noise scaled by the 720 m/s exchange.
  check(std::fabs(leftB->velocity.x + rightB->velocity.x) <= 0.1F,
        "Head-on rebound conserves pair momentum");
}

/// The same closing pair one resolve later: the gate must accept the pair
/// through the resolve-published snapshot (relative displacement covers
/// the gap even though each body's own path falls short).
static void test_head_on_with_resolved_snapshot() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto left = add_fast_sphere(*world, 0.0F, 360.0F);
  const auto right = add_fast_sphere(*world, 22.0F, -360.0F);

  // Frame 1 closes 12 m of the 22 m gap and publishes the snapshot.
  run_step(*world);
  // Frame 2 must catch the crossing through the snapshot gate.
  run_step(*world);

  engine::runtime::Transform leftT{};
  engine::runtime::Transform rightT{};
  const engine::runtime::RigidBody *leftB = world->get_rigid_body_ptr(left);
  const engine::runtime::RigidBody *rightB = world->get_rigid_body_ptr(right);
  if (!world->get_transform(left, &leftT) ||
      !world->get_transform(right, &rightT) || (leftB == nullptr) ||
      (rightB == nullptr)) {
    check(false, "Body lookup failed");
    return;
  }
  check(leftT.position.x < rightT.position.x,
        "Snapshot-gated head-on pair does not swap sides");
  check(leftB->velocity.x < 0.0F, "Snapshot-gated left body rebounds");
  check(rightB->velocity.x > 0.0F, "Snapshot-gated right body rebounds");

  // Direct sweep against the resolved snapshot must report the other body.
  world->begin_update_phase();
  engine::runtime::Transform probeT{};
  const bool haveProbe = world->get_transform(left, &probeT);
  const engine::runtime::RigidBody *probeBody =
      world->get_rigid_body_ptr(left);
  const engine::runtime::Collider *probeCollider =
      world->get_collider_ptr(left);
  bool reported = false;
  if (haveProbe && (probeBody != nullptr) && (probeCollider != nullptr)) {
    engine::runtime::RigidBody approach = *probeBody;
    approach.velocity = engine::math::Vec3(360.0F, 0.0F, 0.0F);
    const engine::physics::CcdSweepResult sweep =
        engine::physics::bilateral_advance_ccd(*world, left, approach,
                                               *probeCollider, probeT,
                                               1.0F / 60.0F);
    reported = sweep.hit && (sweep.hitEntityIndex == right.index);
  }
  check(reported, "Sweep against snapshot reports the opposing body");
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
}

/// Boundary: two fast bodies moving apart share the same relative-motion
/// gate but must never report a hit — both advance their full step.
static void test_separating_pair_full_advance() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto left = add_fast_sphere(*world, 0.0F, -360.0F);
  const auto right = add_fast_sphere(*world, 2.0F, 360.0F);

  run_step(*world);

  engine::runtime::Transform leftT{};
  engine::runtime::Transform rightT{};
  if (!world->get_transform(left, &leftT) ||
      !world->get_transform(right, &rightT)) {
    check(false, "Body lookup failed");
    return;
  }
  check(std::fabs(leftT.position.x + 6.0F) <= 1.0e-3F,
        "Separating left body advances the full step");
  check(std::fabs(rightT.position.x - 8.0F) <= 1.0e-3F,
        "Separating right body advances the full step");
}

/// Boundary: a fast body closing on a slow mover (below the CCD threshold)
/// must still clamp instead of passing through, with the discrete solver
/// finishing the momentum exchange.
static void test_fast_into_slow_mover_no_tunnel() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto fast = add_fast_sphere(*world, 0.0F, 300.0F);
  const auto slow = add_fast_sphere(*world, 4.0F, -1.0F);

  for (int i = 0; i < 2; ++i) {
    run_step(*world);
  }

  engine::runtime::Transform fastT{};
  engine::runtime::Transform slowT{};
  if (!world->get_transform(fast, &fastT) ||
      !world->get_transform(slow, &slowT)) {
    check(false, "Body lookup failed");
    return;
  }
  check(fastT.position.x < slowT.position.x,
        "Fast body never passes through the slow mover");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== CCD Relative Motion Tests (issue #106) ===\n");

  test_head_on_first_step_no_snapshot();
  test_head_on_with_resolved_snapshot();
  test_separating_pair_full_advance();
  test_fast_into_slow_mover_no_tunnel();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
