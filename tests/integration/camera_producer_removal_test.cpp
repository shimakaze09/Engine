// Verifies that removing an entity's camera producers revokes its published
// CameraManager entry (#392): the update passes visit only components that
// still exist, so the World's component removal is the one place that can
// see a producer disappear while the entity stays alive. A removal that
// leaves the other producer standing must keep the entry, and a failed
// removal must not touch a camera published by someone else (a Lua push).

#include <cstdio>
#include <memory>
#include <new>

#include "engine/runtime/camera_component_update.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/spring_arm_update.h"
#include "engine/runtime/world.h"

namespace {

using namespace engine;
using namespace engine::runtime;

/// Publishes one authored CameraComponent camera and returns the entity, or
/// kInvalidEntity when any setup step fails.
Entity publish_component_camera(World &world, float priority) noexcept {
  const Entity entity = world.create_scene_object();
  if (entity == kInvalidEntity) {
    return kInvalidEntity;
  }
  CameraComponent camera{};
  camera.priority = priority;
  if (!world.add_camera_component(entity, camera)) {
    return kInvalidEntity;
  }
  update_persistent_cameras(world, 1.0F);
  if (world.camera_manager().camera_count() != 1U) {
    return kInvalidEntity;
  }
  return entity;
}

/// EXPECTATION (#392): removing a lone CameraComponent revokes the manager
/// entry, and later updates do not resurrect it.
bool test_remove_lone_camera_component_revokes_entry() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  const Entity entity = publish_component_camera(*world, 3.0F);
  if (entity == kInvalidEntity) {
    return false;
  }

  if (!world->remove_camera_component(entity)) {
    return false;
  }
  if (world->camera_manager().camera_count() != 0U) {
    return false;
  }
  if (world->camera_manager().active_camera() != nullptr) {
    return false;
  }

  update_persistent_cameras(*world, 1.0F);
  update_spring_arm_cameras(*world, 1.0F);
  return world->camera_manager().camera_count() == 0U;
}

/// EXPECTATION (#392): removing a spring-arm-only producer revokes the
/// manager entry the same way.
bool test_remove_lone_spring_arm_revokes_entry() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return false;
  }
  SpringArmComponent arm{};
  arm.collisionEnabled = false;
  if (!world->add_spring_arm(entity, arm)) {
    return false;
  }
  update_spring_arm_cameras(*world, 1.0F);
  if (world->camera_manager().camera_count() != 1U) {
    return false;
  }

  if (!world->remove_spring_arm(entity)) {
    return false;
  }
  if (world->camera_manager().camera_count() != 0U) {
    return false;
  }

  update_spring_arm_cameras(*world, 1.0F);
  return world->camera_manager().camera_count() == 0U;
}

/// EXPECTATION (#392): on the combined rig, removing the CameraComponent
/// keeps the entry — the spring arm still publishes the owner (falling back
/// to its default lens/priority on the next update).
bool test_remove_camera_component_keeps_spring_arm_producer() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return false;
  }
  SpringArmComponent arm{};
  arm.collisionEnabled = false;
  if (!world->add_spring_arm(entity, arm)) {
    return false;
  }
  CameraComponent camera{};
  camera.priority = 42.0F;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }
  update_spring_arm_cameras(*world, 1.0F);
  if (world->camera_manager().camera_count() != 1U) {
    return false;
  }

  if (!world->remove_camera_component(entity)) {
    return false;
  }
  if (world->camera_manager().camera_count() != 1U) {
    return false;
  }

  update_spring_arm_cameras(*world, 1.0F);
  const CameraEntry *active = world->camera_manager().active_camera();
  // The next republish drops back to the spring arm's default priority (10)
  // now that no CameraComponent supplies one.
  return (active != nullptr) && (active->ownerEntity == entity) &&
         (active->priority == 10.0F);
}

/// EXPECTATION (#392): on the combined rig, removing the SpringArmComponent
/// keeps the entry — the CameraComponent takes over publishing on the next
/// update with its own lens and priority.
bool test_remove_spring_arm_keeps_camera_component_producer() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return false;
  }
  SpringArmComponent arm{};
  arm.collisionEnabled = false;
  if (!world->add_spring_arm(entity, arm)) {
    return false;
  }
  CameraComponent camera{};
  camera.priority = 42.0F;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }
  update_spring_arm_cameras(*world, 1.0F);
  if (world->camera_manager().camera_count() != 1U) {
    return false;
  }

  if (!world->remove_spring_arm(entity)) {
    return false;
  }
  if (world->camera_manager().camera_count() != 1U) {
    return false;
  }

  update_persistent_cameras(*world, 1.0F);
  const CameraEntry *active = world->camera_manager().active_camera();
  return (active != nullptr) && (active->ownerEntity == entity) &&
         (active->priority == 42.0F);
}

/// EXPECTATION (#392): remove followed by re-add within one Input phase
/// republishes cleanly on the next update — one entry, the new component's
/// priority, no leftover state from the revoked entry.
bool test_remove_then_readd_in_one_phase() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  const Entity entity = publish_component_camera(*world, 3.0F);
  if (entity == kInvalidEntity) {
    return false;
  }

  if (!world->remove_camera_component(entity)) {
    return false;
  }
  CameraComponent replacement{};
  replacement.priority = 9.0F;
  if (!world->add_camera_component(entity, replacement)) {
    return false;
  }
  if (world->camera_manager().camera_count() != 0U) {
    return false; // revoked at removal; republish happens on update
  }

  update_persistent_cameras(*world, 1.0F);
  const CameraEntry *active = world->camera_manager().active_camera();
  return (active != nullptr) && (active->ownerEntity == entity) &&
         (active->priority == 9.0F) &&
         (world->camera_manager().camera_count() == 1U);
}

/// EXPECTATION (#392): a removal that removes nothing revokes nothing — a
/// camera pushed for an entity that never had producer components (the Lua
/// push shape) survives a failed remove call for that entity.
bool test_failed_removal_leaves_pushed_camera() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return false;
  }
  CameraEntry entry{};
  if (!world->camera_manager().push_camera(entity, entry, 1.0F)) {
    return false;
  }

  if (world->remove_camera_component(entity) ||
      world->remove_spring_arm(entity)) {
    return false; // neither component exists, so both removals must fail
  }
  return world->camera_manager().camera_count() == 1U;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int failures = 0;

  const auto run = [&failures](const char *name, bool (*fn)() noexcept) {
    if (!fn()) {
      std::printf("FAIL: %s\n", name);
      ++failures;
    } else {
      std::printf("PASS: %s\n", name);
    }
  };

  run("test_remove_lone_camera_component_revokes_entry",
      test_remove_lone_camera_component_revokes_entry);
  run("test_remove_lone_spring_arm_revokes_entry",
      test_remove_lone_spring_arm_revokes_entry);
  run("test_remove_camera_component_keeps_spring_arm_producer",
      test_remove_camera_component_keeps_spring_arm_producer);
  run("test_remove_spring_arm_keeps_camera_component_producer",
      test_remove_spring_arm_keeps_camera_component_producer);
  run("test_remove_then_readd_in_one_phase",
      test_remove_then_readd_in_one_phase);
  run("test_failed_removal_leaves_pushed_camera",
      test_failed_removal_leaves_pushed_camera);

  if (failures > 0) {
    std::printf("\n%d test(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nAll camera producer removal tests passed.\n");
  return 0;
}
