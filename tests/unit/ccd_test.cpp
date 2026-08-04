// CCD (Continuous Collision Detection) tests — P1-M3-E1d.
// Verifies bilateral advancement prevents tunneling at high speeds.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/ccd.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
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

/// Verifies CCD sees a thin collider through parent TRS and local offset.
static void test_ccd_parented_rotated_scaled_wall() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "Parented CCD world allocation");
    return;
  }
  world->end_frame_phase();

  const auto bullet = world->create_entity();
  engine::runtime::Transform bulletTransform{};
  world->add_transform(bullet, bulletTransform);
  engine::runtime::Collider bulletCollider{};
  bulletCollider.shape = engine::runtime::ColliderShape::Sphere;
  bulletCollider.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  world->add_collider(bullet, bulletCollider);
  engine::runtime::RigidBody bulletBody{};
  bulletBody.inverseMass = 1.0F;
  bulletBody.velocity = engine::math::Vec3(300.0F, 0.0F, 0.0F);
  world->add_rigid_body(bullet, bulletBody);

  constexpr float kQuarterTurnSinCos = 0.7071067811865475F;
  const auto wallParent = world->create_entity();
  engine::runtime::Transform parentTransform{};
  parentTransform.position = engine::math::Vec3(6.0F, 0.0F, 0.0F);
  parentTransform.rotation =
      engine::math::Quat(0.0F, 0.0F, kQuarterTurnSinCos, kQuarterTurnSinCos);
  parentTransform.scale = engine::math::Vec3(2.0F, 1.0F, 1.0F);
  world->add_transform(wallParent, parentTransform);

  const auto wall = world->create_entity();
  engine::runtime::Transform wallTransform{};
  wallTransform.parentId = world->persistent_id(wallParent);
  world->add_transform(wall, wallTransform);
  engine::runtime::Collider wallCollider{};
  wallCollider.halfExtents = engine::math::Vec3(2.0F, 0.02F, 2.0F);
  wallCollider.localPosition = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  world->add_collider(wall, wallCollider);

  const engine::physics::CcdSweepResult hit =
      engine::physics::bilateral_advance_ccd(*world, bullet, bulletBody,
                                             bulletCollider, bulletTransform,
                                             1.0F / 60.0F);
  check(hit.hit, "CCD hits parented rotated scaled wall");
  check(hit.hitEntityIndex == wall.index,
        "CCD reports parented collider entity");
  check(std::fabs(hit.timeOfImpact - 0.976F) <= 1.0e-3F,
        "CCD parented wall time of impact");
}

/// Verifies a collider-less body root sweeps child colliders without self-hits.
static void test_ccd_compound_child_clamps_root() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "Compound CCD world allocation");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  constexpr float kQuarterTurnSinCos = 0.7071067811865475F;
  const engine::math::Quat quarterTurn(0.0F, 0.0F, kQuarterTurnSinCos,
                                       kQuarterTurnSinCos);
  const auto root = world->create_entity();
  engine::runtime::Transform rootTransform{};
  rootTransform.rotation = quarterTurn;
  rootTransform.scale = engine::math::Vec3(2.0F, 1.0F, 1.0F);
  world->add_transform(root, rootTransform);
  engine::runtime::RigidBody rootBody{};
  rootBody.inverseMass = 1.0F;
  rootBody.velocity = engine::math::Vec3(300.0F, 0.0F, 0.0F);
  world->add_rigid_body(root, rootBody);

  const auto movingChild = world->create_entity();
  engine::runtime::Transform childTransform{};
  childTransform.position = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  childTransform.scale = engine::math::Vec3(0.5F, 2.0F, 1.0F);
  childTransform.parentId = world->persistent_id(root);
  world->add_transform(movingChild, childTransform);
  engine::runtime::Collider childCollider{};
  childCollider.localPosition = engine::math::Vec3(0.0F, -0.5F, 0.0F);
  childCollider.localRotation = quarterTurn;
  childCollider.halfExtents = engine::math::Vec3(0.25F, 0.5F, 0.5F);
  world->add_collider(movingChild, childCollider);

  const auto sameRootSibling = world->create_entity();
  engine::runtime::Transform siblingTransform{};
  siblingTransform.parentId = world->persistent_id(root);
  world->add_transform(sameRootSibling, siblingTransform);
  engine::runtime::Collider siblingCollider{};
  siblingCollider.shape = engine::runtime::ColliderShape::Sphere;
  siblingCollider.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  world->add_collider(sameRootSibling, siblingCollider);

  const auto wall = world->create_entity();
  engine::runtime::Transform wallTransform{};
  wallTransform.position = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  world->add_transform(wall, wallTransform);
  engine::runtime::Collider wallCollider{};
  wallCollider.halfExtents = engine::math::Vec3(0.02F, 2.0F, 2.0F);
  world->add_collider(wall, wallCollider);

  world->begin_update_phase();
  engine::runtime::step_physics(*world, 1.0F / 60.0F);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform result{};
  const bool hasResult = world->get_transform(root, &result);
  check(world->get_collider_ptr(root) == nullptr,
        "Compound CCD root remains collider-less");
  check(hasResult && (std::fabs(result.position.x - 4.4301F) <= 2.0e-3F),
        "Compound CCD clamps the rigid-body root at child impact");
  check(hasResult && (result.position.x > 4.0F),
        "Compound CCD skips overlapping same-root colliders");
}

/// H-07 regression: CCD snapshot entries must be matched by entity
/// identity, not dense position. Frames 1-4 run mover M (30 m/s) chasing
/// target T (28 m/s, snapshot velocity captured each resolve) with filler
/// W first in the collider set; the surface gap shrinks from 0.6 to
/// 0.467, below M's 0.5 m static-assumption travel but far above the
/// 0.033 m true relative travel. Removing W's collider then swap-and-pop
/// reorders the dense array. On the fixed code frame 5 finds T's snapshot
/// entry by identity, sees ~0 relative speed, and reports no impact: M's
/// velocity stays exactly +30 (CCD either reflects it to -30 or leaves it
/// untouched, so exact equality is the correct assert). Position must
/// advance a full step (> +0.48 of the 0.5 travel; the clamped path
/// stops at safeToi = 0.923 of the step, i.e. +0.46).
static void test_ccd_snapshot_survives_collider_reorder() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "Reorder CCD world allocation");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto filler = world->create_entity();
  engine::runtime::Transform fillerT{};
  fillerT.position = engine::math::Vec3(-50.0F, 0.0F, 0.0F);
  world->add_transform(filler, fillerT);
  engine::runtime::Collider fillerCol{};
  fillerCol.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);
  world->add_collider(filler, fillerCol);

  const auto mover = world->create_entity();
  engine::runtime::Transform moverT{};
  world->add_transform(mover, moverT);
  engine::runtime::Collider moverCol{};
  moverCol.shape = engine::runtime::ColliderShape::Sphere;
  moverCol.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);
  world->add_collider(mover, moverCol);
  engine::runtime::RigidBody moverRB{};
  moverRB.inverseMass = 1.0F;
  moverRB.velocity = engine::math::Vec3(30.0F, 0.0F, 0.0F);
  world->add_rigid_body(mover, moverRB);

  const auto target = world->create_entity();
  engine::runtime::Transform targetT{};
  targetT.position = engine::math::Vec3(1.1F, 0.0F, 0.0F);
  world->add_transform(target, targetT);
  engine::runtime::Collider targetCol{};
  targetCol.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);
  world->add_collider(target, targetCol);
  engine::runtime::RigidBody targetRB{};
  targetRB.inverseMass = 1.0F;
  targetRB.velocity = engine::math::Vec3(28.0F, 0.0F, 0.0F);
  world->add_rigid_body(target, targetRB);

  const float dt = 1.0F / 60.0F;
  for (int frame = 0; frame < 4; ++frame) {
    world->begin_update_phase();
    engine::runtime::step_physics(*world, dt);
    engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  if (!world->remove_collider(filler)) {
    check(false, "Reorder CCD filler collider removal");
    return;
  }

  engine::runtime::Transform beforeT{};
  world->get_transform(mover, &beforeT);

  world->begin_update_phase();
  engine::runtime::step_physics(*world, dt);
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  const engine::runtime::RigidBody *after = world->get_rigid_body_ptr(mover);
  engine::runtime::Transform afterT{};
  world->get_transform(mover, &afterT);
  check((after != nullptr) && (after->velocity.x == 30.0F),
        "CCD snapshot velocity found by identity after reorder");
  check(afterT.position.x - beforeT.position.x > 0.48F,
        "Mover advances a full step after reorder");
}

/// Boundary: colliders at extreme-but-finite coordinates (1e12, where a
/// float ulp is 65536 m and sub-metre geometry is unrepresentable) must
/// quantize through the clamped cell conversion. The contract at this
/// range is termination and determinism — the unclamped float-to-int32
/// cast is UB and the raw cell loops are unbounded — not contact
/// detection, which float precision already forbids here, so the assert
/// is that resolve completes and the bodies remain finite.
static void test_broadphase_far_coordinates() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "Far-coordinate world allocation");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto first = world->create_entity();
  engine::runtime::Transform firstT{};
  firstT.position = engine::math::Vec3(1.0e12F, 0.0F, 0.0F);
  world->add_transform(first, firstT);
  engine::runtime::Collider firstCol{};
  firstCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(first, firstCol);
  engine::runtime::RigidBody firstRB{};
  firstRB.inverseMass = 1.0F;
  world->add_rigid_body(first, firstRB);

  const auto second = world->create_entity();
  engine::runtime::Transform secondT{};
  secondT.position = engine::math::Vec3(1.0e12F, 0.4F, 0.0F);
  world->add_transform(second, secondT);
  engine::runtime::Collider secondCol{};
  secondCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(second, secondCol);

  world->begin_update_phase();
  const bool resolved = engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  check(resolved, "Far-coordinate resolve completes");
  engine::runtime::Transform firstAfter{};
  check(world->get_transform(first, &firstAfter) &&
            std::isfinite(firstAfter.position.x),
        "Far-coordinate body stays finite");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== CCD Tests (P1-M3-E1) ===\n");

  test_bullet_100_no_tunnel();
  test_bullet_300_no_tunnel();
  test_slow_body_normal_integration();
  test_ccd_velocity_reflects_on_hit();
  test_ccd_parented_rotated_scaled_wall();
  test_ccd_compound_child_clamps_root();
  test_ccd_snapshot_survives_collider_reorder();
  test_broadphase_far_coordinates();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
