// Pins when a sleeping body wakes because the world changed around it, not
// because something hit it. A body asleep on a support skips gravity and
// integration, so it must wake when the support disappears (destroyed, or
// moved away) and when gravity changes; before, only a fast contact
// partner or a script velocity write woke it, and a crate whose platform
// was destroyed hung in the air. Each case settles a box on a static
// platform until it sleeps, changes the world through the World API, and
// steps again.

#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

constexpr float kStep = 1.0F / 60.0F;

/// One fixed step through the production update and resolve, leaving the
/// world in its Input phase.
void step(World &world) noexcept {
  world.begin_update_phase();
  engine::runtime::step_physics(world, kStep);
  engine::runtime::resolve_collisions(world);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
}

/// A static platform with a box resting on it, stepped until the box
/// sleeps. False when the scene cannot be built or the box never sleeps.
bool settle_box_on_platform(World &world, Entity *outPlatform,
                            Entity *outBox) noexcept {
  world.end_frame_phase();
  const Entity platform = world.create_entity();
  const Entity box = world.create_entity();
  if ((platform == kInvalidEntity) || (box == kInvalidEntity)) {
    return false;
  }
  engine::runtime::Transform platformTransform{};
  platformTransform.position = engine::math::Vec3(0.0F, -1.0F, 0.0F);
  engine::runtime::Transform boxTransform{};
  boxTransform.position = engine::math::Vec3(0.0F, 0.5F, 0.0F);
  engine::runtime::Collider platformCollider{};
  platformCollider.halfExtents = engine::math::Vec3(4.0F, 1.0F, 4.0F);
  platformCollider.restitution = 0.0F;
  engine::runtime::Collider boxCollider{};
  boxCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  boxCollider.restitution = 0.0F;
  engine::runtime::RigidBody platformBody{};
  platformBody.inverseMass = 0.0F;
  engine::runtime::RigidBody boxBody{};
  boxBody.inverseMass = 1.0F;
  if (!world.add_transform(platform, platformTransform) ||
      !world.add_transform(box, boxTransform) ||
      !world.add_collider(platform, platformCollider) ||
      !world.add_collider(box, boxCollider) ||
      !world.add_rigid_body(platform, platformBody) ||
      !world.add_rigid_body(box, boxBody)) {
    return false;
  }
  for (int frame = 0; frame < 240; ++frame) {
    step(world);
  }
  *outPlatform = platform;
  *outBox = box;
  return engine::runtime::is_sleeping(world, box);
}

float box_height(const World &world, Entity box) noexcept {
  engine::runtime::Transform transform{};
  return world.get_transform(box, &transform) ? transform.position.y : 0.0F;
}

/// A sleeping box stays asleep while nothing changes: the lost-touch wake
/// must not wake a body still resting on its support.
void check_resting_box_stays_asleep() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Entity platform = kInvalidEntity;
  Entity box = kInvalidEntity;
  if ((world == nullptr) || !settle_box_on_platform(*world, &platform, &box)) {
    check(false, "a box on a platform falls asleep");
    return;
  }
  for (int frame = 0; frame < 60; ++frame) {
    step(*world);
  }
  check(engine::runtime::is_sleeping(*world, box),
        "a box resting on an unchanged platform stays asleep");
}

void check_destroyed_support_wakes_the_box() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Entity platform = kInvalidEntity;
  Entity box = kInvalidEntity;
  if ((world == nullptr) || !settle_box_on_platform(*world, &platform, &box)) {
    check(false, "a box on a platform falls asleep");
    return;
  }
  const float rest = box_height(*world, box);
  check(world->destroy_entity(platform), "the platform is destroyed");
  for (int frame = 0; frame < 30; ++frame) {
    step(*world);
  }
  check(!engine::runtime::is_sleeping(*world, box),
        "a box whose platform is destroyed wakes");
  check(box_height(*world, box) < rest - 0.5F,
        "and falls once its platform is gone");
}

void check_moved_support_wakes_the_box() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Entity platform = kInvalidEntity;
  Entity box = kInvalidEntity;
  if ((world == nullptr) || !settle_box_on_platform(*world, &platform, &box)) {
    check(false, "a box on a platform falls asleep");
    return;
  }
  const float rest = box_height(*world, box);
  engine::runtime::Transform away{};
  away.position = engine::math::Vec3(50.0F, -1.0F, 0.0F);
  check(world->add_transform(platform, away), "the platform moves away");
  for (int frame = 0; frame < 30; ++frame) {
    step(*world);
  }
  check(!engine::runtime::is_sleeping(*world, box),
        "a box whose platform moved away wakes");
  check(box_height(*world, box) < rest - 0.5F,
        "and falls where the platform was");
}

void check_gravity_change_wakes_the_box() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Entity platform = kInvalidEntity;
  Entity box = kInvalidEntity;
  if ((world == nullptr) || !settle_box_on_platform(*world, &platform, &box)) {
    check(false, "a box on a platform falls asleep");
    return;
  }
  const float rest = box_height(*world, box);
  // Gravity turned upward: the box must lift off the platform.
  engine::runtime::set_gravity(*world, 0.0F, 9.8F, 0.0F);
  for (int frame = 0; frame < 30; ++frame) {
    step(*world);
  }
  check(box_height(*world, box) > rest + 0.5F,
        "a box asleep under the old gravity rises under the new one");

  // Setting the same gravity again changes nothing and wakes nothing.
  std::unique_ptr<World> calm(new (std::nothrow) World());
  if ((calm == nullptr) || !settle_box_on_platform(*calm, &platform, &box)) {
    check(false, "a second box falls asleep");
    return;
  }
  float gx = 0.0F;
  float gy = 0.0F;
  float gz = 0.0F;
  check(engine::runtime::get_gravity(*calm, &gx, &gy, &gz),
        "the gravity reads back");
  engine::runtime::set_gravity(*calm, gx, gy, gz);
  check(engine::runtime::is_sleeping(*calm, box),
        "setting the gravity it already has wakes nothing");
}

} // namespace

/// Runs this executable or test program.
int main() {
  check_resting_box_stays_asleep();
  check_destroyed_support_wakes_the_box();
  check_moved_support_wakes_the_box();
  check_gravity_change_wakes_the_box();
  if (g_failures != 0) {
    std::fprintf(stderr, "physics_wake_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("physics_wake_test: sleepers wake when their world changes\n");
  return 0;
}
