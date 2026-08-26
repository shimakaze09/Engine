// Repro for the driven-platform slowdown: a velocity-driven near-kinematic
// box (1000:1 mass ratio, Island Hopper moving-platform dimensions) must
// advance at its commanded speed whether or not a light rotation-locked
// capsule rides on top. Measures effective vs commanded displacement with
// and without the rider and probes the CCD sweep directly against the
// settled resting contact.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/physics/ccd.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

static int g_passed = 0;
static int g_failed = 0;

/// Records one named pass/fail result.
static void check(bool condition, const char *name) noexcept {
  if (condition) {
    ++g_passed;
    std::printf("  PASS: %s\n", name);
  } else {
    ++g_failed;
    std::printf("  FAIL: %s\n", name);
  }
}

namespace {

constexpr float kDt = 1.0F / 60.0F;
constexpr float kGravityY = -9.81F;
constexpr float kPlatformInverseMass = 0.001F;
constexpr float kCommandedSpeed = 3.5F;
constexpr int kSettleSteps = 90;
constexpr int kDriveSteps = 240;
constexpr float kPlatformStartY = 2.0F;
constexpr float kCapsuleRadius = 0.3F;
constexpr float kCapsuleHalfHeight = 0.3F;
constexpr float kRiderRestY = kPlatformStartY + 0.1F + kCapsuleHalfHeight +
                              kCapsuleRadius;

/// Per-run measurements for one driven-platform scenario.
struct DriveResult final {
  bool valid = false;
  float platformStartX = 0.0F;
  float platformEndX = 0.0F;
  float riderEndX = 0.0F;
  float riderSettledY = 0.0F;
  float minStepAdvance = 1.0e9F;
  float maxStepAdvance = -1.0e9F;
  int frozenSteps = 0;
  engine::physics::CcdSweepResult settledSweep{};
  engine::runtime::Entity riderEntity = engine::runtime::kInvalidEntity;
};

/// Runs the scenario: an anti-gravity platform box commanded to a constant
/// horizontal velocity every step (mirroring engine.set_velocity from Lua),
/// optionally carrying a resting capsule whose controller zeroes its own
/// horizontal velocity every step (an idle player). Returns per-step and
/// total displacement measurements plus a direct CCD probe of the settled
/// contact.
DriveResult run_driven_platform(bool withRider) noexcept {
  DriveResult result{};
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return result;
  }

  engine::runtime::set_gravity(*world, 0.0F, kGravityY, 0.0F);

  // Platform: island moving-platform footprint, near-kinematic mass, gravity
  // canceled through the body acceleration so it holds altitude like the
  // scripted platform.
  engine::runtime::Transform platformTransform{};
  platformTransform.position =
      engine::math::Vec3(0.0F, kPlatformStartY, 0.0F);
  const engine::runtime::Entity platform =
      world->create_scene_object(platformTransform);
  engine::runtime::Collider platformCollider{};
  platformCollider.halfExtents = engine::math::Vec3(0.9F, 0.1F, 0.9F);
  platformCollider.restitution = 0.05F;
  engine::runtime::RigidBody platformBody{};
  platformBody.inverseMass = kPlatformInverseMass;
  platformBody.inverseInertia = 0.0F;
  platformBody.acceleration = engine::math::Vec3(0.0F, -kGravityY, 0.0F);
  if ((platform == engine::runtime::kInvalidEntity) ||
      !world->add_collider(platform, platformCollider) ||
      !world->add_rigid_body(platform, platformBody)) {
    return result;
  }

  // Rider: rotation-locked capsule 0.6 off-center so its surface grazes the
  // platform's +x face, matching the island player on the moving platform.
  engine::runtime::Entity rider = engine::runtime::kInvalidEntity;
  if (withRider) {
    engine::runtime::Transform riderTransform{};
    riderTransform.position =
        engine::math::Vec3(0.6F, kRiderRestY + 0.01F, 0.0F);
    rider = world->create_scene_object(riderTransform);
    engine::runtime::Collider riderCollider{};
    riderCollider.shape = engine::runtime::ColliderShape::Capsule;
    riderCollider.halfExtents = engine::math::Vec3(
        kCapsuleRadius, kCapsuleHalfHeight, kCapsuleRadius);
    riderCollider.restitution = 0.05F;
    riderCollider.staticFriction = 0.9F;
    riderCollider.dynamicFriction = 0.7F;
    engine::runtime::RigidBody riderBody{};
    riderBody.inverseMass = 1.0F;
    riderBody.inverseInertia = 0.0F;
    if ((rider == engine::runtime::kInvalidEntity) ||
        !world->add_collider(rider, riderCollider) ||
        !world->add_rigid_body(rider, riderBody)) {
      return result;
    }
  }
  result.riderEntity = rider;

  const auto step_once = [&world, platform, rider](float commandX) -> bool {
    engine::runtime::RigidBody *body = world->get_rigid_body_ptr(platform);
    if (body == nullptr) {
      return false;
    }
    body->velocity = engine::math::Vec3(commandX, 0.0F, 0.0F);
    body->sleeping = false;
    body->sleepFrameCount = 0U;
    if (rider != engine::runtime::kInvalidEntity) {
      engine::runtime::RigidBody *riderBody = world->get_rigid_body_ptr(rider);
      if (riderBody == nullptr) {
        return false;
      }
      riderBody->velocity.x = 0.0F;
      riderBody->velocity.z = 0.0F;
      riderBody->sleeping = false;
      riderBody->sleepFrameCount = 0U;
    }

    world->begin_update_phase();
    const bool ok = engine::runtime::step_physics(*world, kDt) &&
                    engine::runtime::resolve_collisions(*world, kDt);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->begin_render_phase();
    world->end_frame_phase();
    return ok;
  };

  for (int i = 0; i < kSettleSteps; ++i) {
    if (!step_once(0.0F)) {
      return result;
    }
  }

  engine::runtime::Transform settled{};
  if (!world->get_transform(platform, &settled)) {
    return result;
  }
  if (rider != engine::runtime::kInvalidEntity) {
    engine::runtime::Transform riderSettled{};
    if (!world->get_transform(rider, &riderSettled)) {
      return result;
    }
    result.riderSettledY = riderSettled.position.y;

    engine::runtime::RigidBody probeBody{};
    probeBody.inverseMass = kPlatformInverseMass;
    probeBody.inverseInertia = 0.0F;
    probeBody.velocity = engine::math::Vec3(kCommandedSpeed, 0.0F, 0.0F);
    result.settledSweep = engine::physics::bilateral_advance_ccd(
        *world, platform, probeBody, platformCollider, settled, kDt);
  }

  result.platformStartX = settled.position.x;
  float previousX = settled.position.x;
  for (int i = 0; i < kDriveSteps; ++i) {
    if (!step_once(kCommandedSpeed)) {
      return result;
    }
    engine::runtime::Transform now{};
    if (!world->get_transform(platform, &now)) {
      return result;
    }
    const float advance = now.position.x - previousX;
    previousX = now.position.x;
    if (advance < result.minStepAdvance) {
      result.minStepAdvance = advance;
    }
    if (advance > result.maxStepAdvance) {
      result.maxStepAdvance = advance;
    }
    if (advance < (0.5F * kCommandedSpeed * kDt)) {
      ++result.frozenSteps;
    }
  }
  result.platformEndX = previousX;

  if (rider != engine::runtime::kInvalidEntity) {
    engine::runtime::Transform riderEnd{};
    if (world->get_transform(rider, &riderEnd)) {
      result.riderEndX = riderEnd.position.x;
    }
  }
  result.valid = true;
  return result;
}

} // namespace

/// Control: with no rider the driven platform must track its command exactly.
static void test_unloaded_platform_tracks_command() noexcept {
  const DriveResult r = run_driven_platform(false);
  check(r.valid, "Unloaded scenario ran");
  if (!r.valid) {
    return;
  }
  const float commanded =
      kCommandedSpeed * static_cast<float>(kDriveSteps) * kDt;
  const float displacement = r.platformEndX - r.platformStartX;
  std::printf("  unloaded: commanded=%.4f effective=%.4f "
              "stepAdvance=[%.5f, %.5f] frozenSteps=%d\n",
              commanded, displacement, r.minStepAdvance, r.maxStepAdvance,
              r.frozenSteps);
  check(std::fabs(displacement - commanded) < 0.01F,
        "Unloaded platform displacement matches command");
  check(r.frozenSteps == 0, "Unloaded platform never freezes");
}

/// A light resting rider must not steal more than its impulse share
/// (1/1000) of the platform's commanded motion.
static void test_ridden_platform_tracks_command() noexcept {
  const DriveResult r = run_driven_platform(true);
  check(r.valid, "Ridden scenario ran");
  if (!r.valid) {
    return;
  }
  check(std::fabs(r.riderSettledY - kRiderRestY) < 0.02F,
        "Rider settled into resting contact on the platform top");
  std::printf("  settled CCD probe: hit=%d toi=%.5f hitEntity=%u "
              "(rider=%u) normal=(%.3f, %.3f, %.3f)\n",
              r.settledSweep.hit ? 1 : 0, r.settledSweep.timeOfImpact,
              r.settledSweep.hitEntityIndex, r.riderEntity.index,
              r.settledSweep.contactNormal.x, r.settledSweep.contactNormal.y,
              r.settledSweep.contactNormal.z);
  const float commanded =
      kCommandedSpeed * static_cast<float>(kDriveSteps) * kDt;
  const float displacement = r.platformEndX - r.platformStartX;
  std::printf("  ridden: commanded=%.4f effective=%.4f "
              "stepAdvance=[%.5f, %.5f] frozenSteps=%d riderEndX=%.4f\n",
              commanded, displacement, r.minStepAdvance, r.maxStepAdvance,
              r.frozenSteps, r.riderEndX);
  check(std::fabs(displacement - commanded) < 0.05F,
        "Ridden platform displacement matches command");
  check(r.frozenSteps == 0, "Ridden platform never freezes");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== Driven platform vs resting rider (1000:1 mass ratio) ===\n");

  test_unloaded_platform_tracks_command();
  test_ridden_platform_tracks_command();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
