// Verifies camera test behavior for the Engine test suite.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/runtime/camera_manager.h"
#include "engine/runtime/spring_arm_update.h"
#include "engine/runtime/world.h"

namespace {

using namespace engine;
using namespace engine::runtime;

constexpr Entity kOwnerA{1U, 1U};
constexpr Entity kOwnerB{2U, 1U};

/// Returns whether two floats are close enough for camera tests.
bool nearly(float lhs, float rhs) noexcept {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

/// Handles test push pop camera.
bool test_push_pop_camera() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  auto &cm = world->camera_manager();

  CameraEntry entry{};
  entry.position = math::Vec3(10.0F, 5.0F, 0.0F);
  entry.target = math::Vec3(0.0F, 0.0F, 0.0F);

  if (!cm.push_camera(kOwnerA, entry, 1.0F)) {
    return false;
  }
  if (cm.camera_count() != 1U) {
    return false;
  }

  const CameraEntry *active = cm.active_camera();
  if (active == nullptr) {
    return false;
  }
  if (active->position.x != 10.0F) {
    return false;
  }

  if (!cm.pop_camera(kOwnerA)) {
    return false;
  }
  if (cm.camera_count() != 0U) {
    return false;
  }
  if (cm.active_camera() != nullptr) {
    return false;
  }

  return true;
}

/// Handles test priority stack.
bool test_priority_stack() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  auto &cm = world->camera_manager();

  CameraEntry low{};
  low.position = math::Vec3(1.0F, 0.0F, 0.0F);
  CameraEntry high{};
  high.position = math::Vec3(99.0F, 0.0F, 0.0F);

  cm.push_camera(kOwnerA, low, 1.0F);
  cm.push_camera(kOwnerB, high, 10.0F);

  if (cm.camera_count() != 2U) {
    return false;
  }

  const CameraEntry *active = cm.active_camera();
  if (active == nullptr) {
    return false;
  }
  // Highest priority should be entity 2.
  if (active->position.x != 99.0F) {
    return false;
  }

  // Pop high-priority camera.
  cm.pop_camera(kOwnerB);
  active = cm.active_camera();
  if ((active == nullptr) || (active->position.x != 1.0F)) {
    return false;
  }

  return true;
}

/// Handles test blend interpolation.
bool test_blend_interpolation() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  auto &cm = world->camera_manager();

  CameraEntry entry{};
  entry.position = math::Vec3(10.0F, 0.0F, 0.0F);
  entry.target = math::Vec3(0.0F, 0.0F, 0.0F);
  entry.blendSpeed = 5.0F;

  cm.push_camera(kOwnerA, entry, 1.0F);

  // First evaluate snaps directly.
  math::Vec3 pos{}, tgt{}, up{};
  float fov = 0.0F, nearP = 0.0F, farP = 0.0F;
  cm.evaluate(0.0F, &pos, &tgt, &up, &fov, &nearP, &farP);
  if (pos.x != 10.0F) {
    return false;
  }

  // Push a second camera and evaluate — should blend.
  CameraEntry entry2{};
  entry2.position = math::Vec3(20.0F, 0.0F, 0.0F);
  entry2.target = math::Vec3(0.0F, 0.0F, 0.0F);
  entry2.blendSpeed = 5.0F;
  cm.push_camera(kOwnerB, entry2, 10.0F);

  cm.evaluate(0.1F, &pos, &tgt, &up, &fov, &nearP, &farP);
  // Position should be moving toward 20 but not there yet.
  if ((pos.x <= 10.0F) || (pos.x >= 20.0F)) {
    return false;
  }

  return true;
}

/// Handles test camera shake nonzero during and zero after.
bool test_camera_shake_nonzero_during_and_zero_after() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  auto &cm = world->camera_manager();

  CameraEntry entry{};
  entry.position = math::Vec3(0.0F, 0.0F, 0.0F);
  entry.target = math::Vec3(0.0F, 0.0F, -1.0F);
  cm.push_camera(kOwnerA, entry, 1.0F);

  cm.add_shake(1.0F, 15.0F, 0.5F, 2.0F);

  // First evaluate (snaps, dt=0 for snap, then apply shake).
  math::Vec3 pos{}, tgt{}, up{};
  float fov = 0.0F, nearP = 0.0F, farP = 0.0F;

  // Evaluate one step to snap camera.
  cm.evaluate(0.0F, &pos, &tgt, &up, &fov, &nearP, &farP);

  // Now evaluate with time so shake advances.
  cm.evaluate(0.1F, &pos, &tgt, &up, &fov, &nearP, &farP);

  // At least one shake axis should be nonzero.
  const float shakeLen =
      std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
  if (shakeLen < 0.001F) {
    std::printf("  shake offset too small during shake: %f\n", shakeLen);
    return false;
  }

  if (cm.shake_count() != 1U) {
    return false;
  }

  // Advance past duration (0.5s total).
  cm.evaluate(0.5F, &pos, &tgt, &up, &fov, &nearP, &farP);

  // Shake should be expired.
  if (cm.shake_count() != 0U) {
    return false;
  }

  // Position should be back to base (0,0,0).
  const float residual =
      std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
  if (residual > 0.001F) {
    std::printf("  residual offset after shake: %f\n", residual);
    return false;
  }

  return true;
}

/// Handles test multiple shakes additive.
bool test_multiple_shakes_additive() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  auto &cm = world->camera_manager();

  CameraEntry entry{};
  entry.position = math::Vec3(0.0F, 0.0F, 0.0F);
  entry.target = math::Vec3(0.0F, 0.0F, -1.0F);
  cm.push_camera(kOwnerA, entry, 1.0F);

  cm.add_shake(0.5F, 10.0F, 1.0F, 1.0F);
  cm.add_shake(0.5F, 20.0F, 1.0F, 1.0F);

  if (cm.shake_count() != 2U) {
    return false;
  }

  // Evaluate snap + time step.
  math::Vec3 pos{}, tgt{}, up{};
  float fov = 0.0F, nearP = 0.0F, farP = 0.0F;
  cm.evaluate(0.0F, &pos, &tgt, &up, &fov, &nearP, &farP);
  cm.evaluate(0.05F, &pos, &tgt, &up, &fov, &nearP, &farP);

  // Offsets should be nonzero (additive of two shakes).
  const float len = std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
  if (len < 0.001F) {
    return false;
  }

  return true;
}

/// Handles test destroyed camera owner cleanup.
bool test_destroyed_owner_removes_camera() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }

  const Entity owner = world->create_entity();
  if (owner == kInvalidEntity) {
    return false;
  }

  CameraEntry entry{};
  entry.position = math::Vec3(4.0F, 5.0F, 6.0F);
  if (!world->camera_manager().push_camera(owner, entry, 1.0F)) {
    return false;
  }
  if (world->camera_manager().active_camera() == nullptr) {
    return false;
  }

  if (!world->destroy_entity(owner)) {
    return false;
  }
  if ((world->camera_manager().camera_count() != 0U) ||
      (world->camera_manager().active_camera() != nullptr)) {
    return false;
  }

  const Entity recycled = world->create_entity();
  if ((recycled == kInvalidEntity) || (recycled.index != owner.index) ||
      (recycled.generation == owner.generation)) {
    return false;
  }

  CameraEntry recycledEntry{};
  recycledEntry.position = math::Vec3(9.0F, 0.0F, 0.0F);
  if (!world->camera_manager().push_camera(recycled, recycledEntry, 1.0F)) {
    return false;
  }
  if (world->camera_manager().pop_camera(owner)) {
    return false;
  }
  const CameraEntry *active = world->camera_manager().active_camera();
  return (active != nullptr) && (active->ownerEntity == recycled);
}

/// Handles test spring arm crud.
bool test_spring_arm_crud() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  const Entity entity = world->create_entity();
  if (entity == kInvalidEntity) {
    return false;
  }

  SpringArmComponent arm{};
  arm.armLength = 8.0F;
  arm.offset = math::Vec3(0.0F, 2.0F, 0.0F);

  if (!world->add_spring_arm(entity, arm)) {
    return false;
  }
  if (!world->has_spring_arm(entity)) {
    return false;
  }

  SpringArmComponent out{};
  if (!world->get_spring_arm(entity, &out)) {
    return false;
  }
  if (out.armLength != 8.0F) {
    return false;
  }
  if (out.offset.y != 2.0F) {
    return false;
  }

  if (!world->remove_spring_arm(entity)) {
    return false;
  }
  if (world->has_spring_arm(entity)) {
    return false;
  }

  return true;
}

/// Handles test spring arm updates camera position.
bool test_spring_arm_updates_camera_position() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  const Entity entity = world->create_entity();
  if (entity == kInvalidEntity) {
    return false;
  }

  Transform transform{};
  transform.position = math::Vec3(1.0F, 2.0F, 3.0F);
  if (!world->add_transform(entity, transform)) {
    return false;
  }

  SpringArmComponent arm{};
  arm.armLength = 8.0F;
  arm.currentLength = 8.0F;
  arm.offset = math::Vec3(0.0F, 2.0F, 0.0F);
  arm.lagSpeed = 100.0F;
  if (!world->add_spring_arm(entity, arm)) {
    return false;
  }

  update_spring_arm_cameras(*world, 1.0F);
  const CameraEntry *active = world->camera_manager().active_camera();
  if ((active == nullptr) || (active->ownerEntity != entity)) {
    return false;
  }

  return nearly(active->target.x, 1.0F) && nearly(active->target.y, 4.0F) &&
         nearly(active->target.z, 3.0F) &&
         nearly(active->position.x, 1.0F) &&
         nearly(active->position.y, 4.0F) &&
         nearly(active->position.z, 11.0F);
}

/// Handles test clear.
bool test_clear() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  auto &cm = world->camera_manager();

  CameraEntry entry{};
  cm.push_camera(kOwnerA, entry, 1.0F);
  cm.add_shake(1.0F, 10.0F, 1.0F, 1.0F);
  cm.clear();

  if (cm.camera_count() != 0U) {
    return false;
  }
  if (cm.shake_count() != 0U) {
    return false;
  }
  return true;
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

  run("test_push_pop_camera", test_push_pop_camera);
  run("test_priority_stack", test_priority_stack);
  run("test_blend_interpolation", test_blend_interpolation);
  run("test_camera_shake_nonzero_then_zero",
      test_camera_shake_nonzero_during_and_zero_after);
  run("test_multiple_shakes_additive", test_multiple_shakes_additive);
  run("test_spring_arm_crud", test_spring_arm_crud);
  run("test_spring_arm_updates_camera_position",
      test_spring_arm_updates_camera_position);
  run("test_destroyed_owner_removes_camera", test_destroyed_owner_removes_camera);
  run("test_clear", test_clear);

  if (failures > 0) {
    std::printf("\n%d test(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nAll camera tests passed.\n");
  return 0;
}
