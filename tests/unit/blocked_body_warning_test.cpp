// Verifies the blocked-body warning diagnostic: a velocity-driven box
// pressed into a static wall warns exactly once per blocking episode after
// physics.blocked_warn_steps consecutive blocked steps, names the blocking
// partner, resets its count when the command pauses, stays silent for
// unobstructed motion, and is disabled by cvar 0.

#include <cstdio>
#include <memory>
#include <new>

#include "engine/core/cvar.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
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
constexpr float kDriveSpeed = 1.5F;
constexpr int kWarnSteps = 30;

/// One driven-box world: a near-kinematic box commanded along +x each step,
/// optionally with a static wall flush against its +x face.
struct DrivenBoxWorld final {
  std::unique_ptr<engine::runtime::World> world;
  engine::runtime::Entity box = engine::runtime::kInvalidEntity;
  engine::runtime::Entity wall = engine::runtime::kInvalidEntity;
};

/// Builds the driven box (and wall when requested) in a zero-gravity world.
DrivenBoxWorld make_driven_box_world(bool withWall) noexcept {
  DrivenBoxWorld setup{};
  setup.world.reset(new (std::nothrow) engine::runtime::World());
  if (setup.world == nullptr) {
    return setup;
  }
  engine::runtime::set_gravity(*setup.world, 0.0F, 0.0F, 0.0F);

  engine::runtime::Transform boxTransform{};
  boxTransform.position = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  setup.box = setup.world->create_scene_object(boxTransform);
  engine::runtime::Collider boxCollider{};
  boxCollider.restitution = 0.0F;
  engine::runtime::RigidBody boxBody{};
  boxBody.inverseMass = 0.001F;
  boxBody.inverseInertia = 0.0F;
  if ((setup.box == engine::runtime::kInvalidEntity) ||
      !setup.world->add_collider(setup.box, boxCollider) ||
      !setup.world->add_rigid_body(setup.box, boxBody)) {
    setup.world.reset();
    return setup;
  }

  if (withWall) {
    engine::runtime::Transform wallTransform{};
    wallTransform.position = engine::math::Vec3(2.0F, 0.0F, 0.0F);
    setup.wall = setup.world->create_scene_object(wallTransform);
    engine::runtime::Collider wallCollider{};
    wallCollider.halfExtents = engine::math::Vec3(0.5F, 2.0F, 2.0F);
    wallCollider.restitution = 0.0F;
    engine::runtime::RigidBody wallBody{};
    wallBody.inverseMass = 0.0F;
    if ((setup.wall == engine::runtime::kInvalidEntity) ||
        !setup.world->add_collider(setup.wall, wallCollider) ||
        !setup.world->add_rigid_body(setup.wall, wallBody)) {
      setup.world.reset();
      return setup;
    }
  }
  return setup;
}

/// Commands the box's velocity and simulates one fixed step.
bool step_once(DrivenBoxWorld &setup, float commandX) noexcept {
  engine::runtime::RigidBody *body =
      setup.world->get_rigid_body_ptr(setup.box);
  if (body == nullptr) {
    return false;
  }
  body->velocity = engine::math::Vec3(commandX, 0.0F, 0.0F);
  body->sleeping = false;
  body->sleepFrameCount = 0U;

  setup.world->begin_update_phase();
  const bool ok = engine::runtime::step_physics(*setup.world, kDt) &&
                  engine::runtime::resolve_collisions(*setup.world, kDt);
  setup.world->commit_update_phase();
  setup.world->begin_render_prep_phase();
  setup.world->begin_render_phase();
  setup.world->end_frame_phase();
  return ok;
}

/// Total warnings currently recorded for the setup's world.
std::uint32_t warning_count(const DrivenBoxWorld &setup) noexcept {
  return engine::physics::blocked_body_warning_stats(*setup.world)
      .totalWarnings;
}

} // namespace

/// Wall-blocked drive: silent below the threshold, exactly one warning at
/// it, no repeat while the episode continues, correct attribution, and a
/// command pause resets the consecutive count.
static void test_blocked_box_warns_once_per_episode() noexcept {
  DrivenBoxWorld setup = make_driven_box_world(true);
  check(setup.world != nullptr, "Blocked scenario world created");
  if (setup.world == nullptr) {
    return;
  }

  bool stepsOk = true;
  for (int i = 0; i < 20; ++i) {
    stepsOk = stepsOk && step_once(setup, kDriveSpeed);
  }
  check(stepsOk && (warning_count(setup) == 0U),
        "No warning after 20 blocked steps");

  for (int i = 0; i < 2; ++i) {
    stepsOk = stepsOk && step_once(setup, 0.0F);
  }
  for (int i = 0; i < kWarnSteps - 1; ++i) {
    stepsOk = stepsOk && step_once(setup, kDriveSpeed);
  }
  check(stepsOk && (warning_count(setup) == 0U),
        "Command pause resets the consecutive-blocked count");

  stepsOk = stepsOk && step_once(setup, kDriveSpeed);
  check(stepsOk && (warning_count(setup) == 1U),
        "Exactly one warning at the threshold step");

  const engine::physics::BlockedBodyWarningStats stats =
      engine::physics::blocked_body_warning_stats(*setup.world);
  check(stats.lastBlockedEntityIndex == setup.box.index,
        "Warning names the blocked body");
  check(stats.lastBlockingEntityIndex == setup.wall.index,
        "Warning names the blocking wall");

  for (int i = 0; i < 100; ++i) {
    stepsOk = stepsOk && step_once(setup, kDriveSpeed);
  }
  check(stepsOk && (warning_count(setup) == 1U),
        "No repeat warning while the episode continues");
}

/// Unobstructed drive stays silent.
static void test_free_box_never_warns() noexcept {
  DrivenBoxWorld setup = make_driven_box_world(false);
  check(setup.world != nullptr, "Free scenario world created");
  if (setup.world == nullptr) {
    return;
  }
  bool stepsOk = true;
  for (int i = 0; i < 120; ++i) {
    stepsOk = stepsOk && step_once(setup, kDriveSpeed);
  }
  check(stepsOk && (warning_count(setup) == 0U),
        "Unobstructed driven box never warns");
}

/// physics.blocked_warn_steps 0 disables the diagnostic entirely.
static void test_cvar_zero_disables() noexcept {
  check(engine::core::cvar_set_float("physics.blocked_warn_steps", 0.0F),
        "Cvar set to 0");
  DrivenBoxWorld setup = make_driven_box_world(true);
  check(setup.world != nullptr, "Disabled scenario world created");
  if (setup.world == nullptr) {
    return;
  }
  bool stepsOk = true;
  for (int i = 0; i < 120; ++i) {
    stepsOk = stepsOk && step_once(setup, kDriveSpeed);
  }
  check(stepsOk && (warning_count(setup) == 0U),
        "Diagnostic disabled at threshold 0");
  check(engine::core::cvar_set_float("physics.blocked_warn_steps",
                                     static_cast<float>(kWarnSteps)),
        "Cvar restored");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== Blocked-body warning diagnostic ===\n");

  check(engine::physics::register_physics_cvars(),
        "Physics cvars registered");

  test_blocked_box_warns_once_per_episode();
  test_free_box_never_warns();
  test_cvar_zero_disables();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
