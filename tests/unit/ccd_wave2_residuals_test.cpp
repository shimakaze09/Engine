// Regression tests for issue #122 (wave-2 CCD residuals from PR #121's
// closure report): (a) a fast dynamic target whose own sweep is gated out by
// its travel/extent gate must not make the mover's sweep assume a symmetric
// share that never lands, and (b) capsule-capsule/capsule-sphere CCD
// contacts must use the exact closest-point-on-axis normal instead of raw
// EPA output.

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

// ---------------------------------------------------------------------------
// (a) Extent-gated fast dynamic target must not trigger a one-sided impulse.
// ---------------------------------------------------------------------------

/// A target moving above physics.ccd_threshold (2.0 m/s default) but whose
/// own travel this step (speed*dt) does not clear half its own smallest
/// extent never runs its own CCD sweep (bilateral_advance_ccd's entry gate),
/// so it can never apply its half of a symmetric impulse. Before the fix,
/// targetRespondsInCcd only checked speed, so the mover's sweep wrongly
/// assumed the target would reciprocate and applied a one-sided partial
/// impulse -- deleting momentum from the pair for that step. Confirms the
/// sweep decision directly: a 1x1x1 m box moving at 5 m/s travels 0.083 m in
/// one 1/60 s step, well under half its 1 m extent, so its own sweep is
/// gated even though 5 m/s clears the speed threshold.
static void test_extent_gated_target_does_not_respond() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto mover = world->create_entity();
  engine::runtime::Transform moverTransform{};
  world->add_transform(mover, moverTransform);
  engine::runtime::Collider moverCollider{};
  moverCollider.shape = engine::runtime::ColliderShape::Sphere;
  moverCollider.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  world->add_collider(mover, moverCollider);
  engine::runtime::RigidBody moverBody{};
  moverBody.inverseMass = 1.0F;
  moverBody.velocity = engine::math::Vec3(200.0F, 0.0F, 0.0F);
  world->add_rigid_body(mover, moverBody);

  // Large-extent dynamic target: fast enough to clear the speed gate (5 >=
  // 2) but its own travel (5/60 = 0.083 m) never clears half its 1 m extent.
  const auto target = world->create_entity();
  engine::runtime::Transform targetTransform{};
  targetTransform.position = engine::math::Vec3(3.0F, 0.0F, 0.0F);
  world->add_transform(target, targetTransform);
  engine::runtime::Collider targetCollider{};
  targetCollider.halfExtents = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  world->add_collider(target, targetCollider);
  engine::runtime::RigidBody targetBody{};
  targetBody.inverseMass = 0.5F;
  targetBody.velocity = engine::math::Vec3(0.0F, 5.0F, 0.0F);
  world->add_rigid_body(target, targetBody);

  // begin_update_phase primes the CCD snapshot from live velocities (fresh
  // world, first step) so the direct sweep below reads the target's real
  // velocity instead of the open-bounds default.
  world->begin_update_phase();

  const engine::physics::CcdSweepResult sweep =
      engine::physics::bilateral_advance_ccd(*world, mover, moverBody,
                                             moverCollider, moverTransform,
                                             1.0F / 60.0F);

  check(sweep.hit, "Sweep detects the large extent-gated target");
  check(sweep.targetInverseMass > 0.0F, "Target reports as dynamic");
  check(!sweep.targetRespondsInCcd,
        "Extent-gated fast target is not assumed to reciprocate");

  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
}

/// Positive control: same speed (5 m/s, clears the threshold) but a small
/// 0.1 m extent, so the target's own travel (0.083 m) DOES clear half its
/// extent (0.05 m) and its own sweep would run. targetRespondsInCcd must
/// stay true here -- confirms the extent check does not regress the
/// legitimate symmetric-response case.
static void test_small_extent_fast_target_still_responds() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto mover = world->create_entity();
  engine::runtime::Transform moverTransform{};
  world->add_transform(mover, moverTransform);
  engine::runtime::Collider moverCollider{};
  moverCollider.shape = engine::runtime::ColliderShape::Sphere;
  moverCollider.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  world->add_collider(mover, moverCollider);
  engine::runtime::RigidBody moverBody{};
  moverBody.inverseMass = 1.0F;
  moverBody.velocity = engine::math::Vec3(200.0F, 0.0F, 0.0F);
  world->add_rigid_body(mover, moverBody);

  const auto target = world->create_entity();
  engine::runtime::Transform targetTransform{};
  targetTransform.position = engine::math::Vec3(3.0F, 0.0F, 0.0F);
  world->add_transform(target, targetTransform);
  engine::runtime::Collider targetCollider{};
  targetCollider.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  world->add_collider(target, targetCollider);
  engine::runtime::RigidBody targetBody{};
  targetBody.inverseMass = 0.5F;
  targetBody.velocity = engine::math::Vec3(0.0F, 5.0F, 0.0F);
  world->add_rigid_body(target, targetBody);

  world->begin_update_phase();

  const engine::physics::CcdSweepResult sweep =
      engine::physics::bilateral_advance_ccd(*world, mover, moverBody,
                                             moverCollider, moverTransform,
                                             1.0F / 60.0F);

  check(sweep.hit, "Sweep detects the small fast target");
  check(sweep.targetInverseMass > 0.0F, "Target reports as dynamic");
  check(sweep.targetRespondsInCcd,
        "Small-extent fast target still assumed to reciprocate");

  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
}

/// Production-path check: the extent-gated scenario above must not leave the
/// mover with a one-sided partial velocity change after step_physics alone
/// (before resolve_collisions runs). A one-sided impulse response would
/// change moverBody's X velocity from the full 200 m/s approach speed to a
/// partial value; deferring to the discrete solver (which safely updates
/// both bodies in its serial pass) must leave the CCD-only step momentum
/// neutral instead.
static void test_extent_gated_target_no_one_sided_velocity_change() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto mover = world->create_entity();
  engine::runtime::Transform moverTransform{};
  world->add_transform(mover, moverTransform);
  engine::runtime::Collider moverCollider{};
  moverCollider.shape = engine::runtime::ColliderShape::Sphere;
  moverCollider.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  world->add_collider(mover, moverCollider);
  engine::runtime::RigidBody moverBody{};
  moverBody.inverseMass = 1.0F;
  moverBody.velocity = engine::math::Vec3(200.0F, 0.0F, 0.0F);
  world->add_rigid_body(mover, moverBody);

  const auto target = world->create_entity();
  engine::runtime::Transform targetTransform{};
  targetTransform.position = engine::math::Vec3(3.0F, 0.0F, 0.0F);
  world->add_transform(target, targetTransform);
  engine::runtime::Collider targetCollider{};
  targetCollider.halfExtents = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  world->add_collider(target, targetCollider);
  engine::runtime::RigidBody targetBody{};
  targetBody.inverseMass = 0.5F;
  targetBody.velocity = engine::math::Vec3(0.0F, 5.0F, 0.0F);
  world->add_rigid_body(target, targetBody);

  world->begin_update_phase();
  engine::runtime::step_physics(*world, 1.0F / 60.0F);
  // No resolve_collisions here: isolates the CCD-only response.
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  const engine::runtime::RigidBody *moverAfter =
      world->get_rigid_body_ptr(mover);
  check(moverAfter != nullptr, "Mover body lookup succeeded");
  check(moverAfter != nullptr &&
            (std::fabs(moverAfter->velocity.x - 200.0F) <= 1.0e-3F),
        "CCD alone leaves the mover's velocity unchanged against an "
        "extent-gated target (no one-sided impulse)");
}

// ---------------------------------------------------------------------------
// (b) Capsule CCD contacts use the exact closest-point-on-axis normal.
// ---------------------------------------------------------------------------

/// Two Y-aligned capsules approaching head-on along X: by symmetry (both
/// segments share the same Y range, offset only along X) the true contact
/// normal is exactly the X axis with zero Y/Z component. Raw EPA normals on
/// curved-curved polytopes are documented as tens of degrees off; the exact
/// closest-point-between-segments treatment gives Y=Z=0 to float precision
/// regardless of where along the shared Y range contact is queried.
static void test_capsule_capsule_head_on_normal_is_axis_exact() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto mover = world->create_entity();
  engine::runtime::Transform moverTransform{};
  world->add_transform(mover, moverTransform);
  engine::runtime::Collider moverCollider{};
  moverCollider.shape = engine::runtime::ColliderShape::Capsule;
  moverCollider.halfExtents = engine::math::Vec3(0.3F, 1.0F, 0.3F);
  world->add_collider(mover, moverCollider);
  engine::runtime::RigidBody moverBody{};
  moverBody.inverseMass = 1.0F;
  moverBody.velocity = engine::math::Vec3(200.0F, 0.0F, 0.0F);
  world->add_rigid_body(mover, moverBody);

  const auto target = world->create_entity();
  engine::runtime::Transform targetTransform{};
  targetTransform.position = engine::math::Vec3(2.5F, 0.0F, 0.0F);
  world->add_transform(target, targetTransform);
  engine::runtime::Collider targetCollider{};
  targetCollider.shape = engine::runtime::ColliderShape::Capsule;
  targetCollider.halfExtents = engine::math::Vec3(0.3F, 1.0F, 0.3F);
  world->add_collider(target, targetCollider);

  world->begin_update_phase();

  const engine::physics::CcdSweepResult sweep =
      engine::physics::bilateral_advance_ccd(*world, mover, moverBody,
                                             moverCollider, moverTransform,
                                             1.0F / 60.0F);

  check(sweep.hit, "Sweep detects the aligned capsule target");
  check(sweep.hit && (std::fabs(sweep.contactNormal.y) <= 1.0e-4F),
        "Capsule-capsule head-on normal has no Y component");
  check(sweep.hit && (std::fabs(sweep.contactNormal.z) <= 1.0e-4F),
        "Capsule-capsule head-on normal has no Z component");
  check(sweep.hit && (sweep.contactNormal.x < -0.999F),
        "Capsule-capsule head-on normal points back along the approach axis");

  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
}

/// A capsule mover approaching a sphere positioned off the capsule's own
/// core axis (offset along Z): the exact normal must point from the
/// sphere's center toward the CLOSEST point on the capsule's segment, not
/// the capsule's own center -- the two differ whenever the sphere sits
/// beside the capsule's axis rather than in front of an end cap.
static void test_capsule_sphere_normal_uses_segment_not_center() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  // Capsule mover: segment spans y in [-1, 1] at x=0, z sweeps toward the
  // sphere.
  const auto mover = world->create_entity();
  engine::runtime::Transform moverTransform{};
  world->add_transform(mover, moverTransform);
  engine::runtime::Collider moverCollider{};
  moverCollider.shape = engine::runtime::ColliderShape::Capsule;
  moverCollider.halfExtents = engine::math::Vec3(0.3F, 1.0F, 0.3F);
  world->add_collider(mover, moverCollider);
  engine::runtime::RigidBody moverBody{};
  moverBody.inverseMass = 1.0F;
  moverBody.velocity = engine::math::Vec3(0.0F, 0.0F, 200.0F);
  world->add_rigid_body(mover, moverBody);

  // Static sphere offset near one end of the capsule's segment (y=0.9) so
  // the closest point on the segment is NOT the capsule's own center
  // (y=0); a center-based normal and a segment-based normal diverge in Y.
  const auto target = world->create_entity();
  engine::runtime::Transform targetTransform{};
  targetTransform.position = engine::math::Vec3(0.0F, 0.9F, 2.5F);
  world->add_transform(target, targetTransform);
  engine::runtime::Collider targetCollider{};
  targetCollider.shape = engine::runtime::ColliderShape::Sphere;
  targetCollider.halfExtents = engine::math::Vec3(0.3F, 0.3F, 0.3F);
  world->add_collider(target, targetCollider);

  world->begin_update_phase();

  const engine::physics::CcdSweepResult sweep =
      engine::physics::bilateral_advance_ccd(*world, mover, moverBody,
                                             moverCollider, moverTransform,
                                             1.0F / 60.0F);

  check(sweep.hit, "Sweep detects the offset sphere target");
  // The exact segment-closest-point normal is purely along Z (the sphere
  // sits directly ahead of the segment point y=0.9 along the capsule's own
  // Y range, so no lateral Y correction is exact); a center-based
  // approximation would instead point partly toward y=0 and pick up a
  // spurious negative Y component of roughly -0.9 in 2.5 (about -0.34
  // normalized), which the exact treatment must not reproduce.
  check(sweep.hit && (std::fabs(sweep.contactNormal.y) <= 1.0e-3F),
        "Capsule-sphere normal follows the segment's closest point, not "
        "the capsule's own center");

  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
}

/// Runs this executable or test program.
int main() {
  std::printf("=== CCD Wave-2 Residual Tests (issue #122) ===\n");

  test_extent_gated_target_does_not_respond();
  test_small_extent_fast_target_still_responds();
  test_extent_gated_target_no_one_sided_velocity_change();
  test_capsule_capsule_head_on_normal_is_axis_exact();
  test_capsule_sphere_normal_uses_segment_not_center();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
