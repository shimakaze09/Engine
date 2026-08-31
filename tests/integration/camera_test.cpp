// Verifies camera test behavior for the Engine test suite.

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>

#include "engine/runtime/camera_component_update.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/scene_serializer.h"
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

  cm.pop_camera(kOwnerB);
  active = cm.active_camera();
  if ((active == nullptr) || (active->position.x != 1.0F)) {
    return false;
  }

  return true;
}

bool test_blend_interpolation() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  auto &cm = world->camera_manager();

  CameraEntry entry{};
  entry.position = math::Vec3(10.0F, 0.0F, 0.0F);
  entry.target = math::Vec3(0.0F, 0.0F, 0.0F);
  entry.blendSpeed = 5.0F;

  cm.push_camera(kOwnerA, entry, 1.0F);

  math::Vec3 pos{}, tgt{}, up{};
  float fov = 0.0F, nearP = 0.0F, farP = 0.0F;
  cm.evaluate(0.0F, &pos, &tgt, &up, &fov, &nearP, &farP);
  if (pos.x != 10.0F) {
    return false;
  }

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

bool test_camera_shake_large_phase_is_finite_and_deterministic() noexcept {
  CameraManager first{};
  CameraManager second{};

  CameraEntry entry{};
  entry.position = math::Vec3(0.0F, 0.0F, 0.0F);
  entry.target = math::Vec3(0.0F, 0.0F, -1.0F);
  if (!first.push_camera(kOwnerA, entry, 1.0F) ||
      !second.push_camera(kOwnerA, entry, 1.0F)) {
    return false;
  }

  constexpr float kHugeFrequency = 1.0e30F;
  if (!first.add_shake(1.0F, kHugeFrequency, 1.0F, 0.0F) ||
      !second.add_shake(1.0F, kHugeFrequency, 1.0F, 0.0F)) {
    return false;
  }

  math::Vec3 posA{}, targetA{}, upA{};
  math::Vec3 posB{}, targetB{}, upB{};
  float fovA = 0.0F, nearA = 0.0F, farA = 0.0F;
  float fovB = 0.0F, nearB = 0.0F, farB = 0.0F;
  first.evaluate(0.0F, &posA, &targetA, &upA, &fovA, &nearA, &farA);
  second.evaluate(0.0F, &posB, &targetB, &upB, &fovB, &nearB, &farB);
  first.evaluate(0.25F, &posA, &targetA, &upA, &fovA, &nearA, &farA);
  second.evaluate(0.25F, &posB, &targetB, &upB, &fovB, &nearB, &farB);

  if (!std::isfinite(posA.x) || !std::isfinite(posA.y) ||
      !std::isfinite(posA.z) || !std::isfinite(targetA.x) ||
      !std::isfinite(targetA.y) || !std::isfinite(targetA.z)) {
    return false;
  }

  return (posA.x == posB.x) && (posA.y == posB.y) && (posA.z == posB.z) &&
         (targetA.x == targetB.x) && (targetA.y == targetB.y) &&
         (targetA.z == targetB.z) && (upA.x == upB.x) && (upA.y == upB.y) &&
         (upA.z == upB.z) && (fovA == fovB) && (nearA == nearB) &&
         (farA == farB);
}

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


/// Regression for issue #393: the manager boundary refuses non-finite and
/// out-of-range camera and shake parameters, and a refusal mutates
/// nothing — a live entry keeps its values and the evaluated camera stays
/// finite and unchanged.
bool test_manager_rejects_invalid_parameters() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  auto &cm = world->camera_manager();
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  const float kInf = std::numeric_limits<float>::infinity();

  // Baseline valid camera.
  CameraEntry valid{};
  valid.position = math::Vec3(3.0F, 2.0F, 1.0F);
  valid.target = math::Vec3(0.0F, 0.0F, 0.0F);
  if (!cm.push_camera(kOwnerA, valid, 1.0F)) {
    return false;
  }

  // Each invalid push must return false, add nothing, and leave owner A's
  // live entry untouched — including the update path, where the same
  // owner's refused push must not partially overwrite the live fields.
  CameraEntry nanPosition = valid;
  nanPosition.position.y = kNan;
  CameraEntry infTarget = valid;
  infTarget.target.z = kInf;
  CameraEntry nanUp = valid;
  nanUp.up.x = kNan;
  CameraEntry nanFov = valid;
  nanFov.fovRadians = kNan;
  CameraEntry zeroNear = valid;
  zeroNear.nearPlane = 0.0F;
  CameraEntry negativeNear = valid;
  negativeNear.nearPlane = -0.1F;
  CameraEntry farInsideNear = valid;
  farInsideNear.farPlane = valid.nearPlane;
  CameraEntry infFar = valid;
  infFar.farPlane = kInf;
  CameraEntry negativeBlend = valid;
  negativeBlend.blendSpeed = -1.0F;
  CameraEntry zeroOrthoSize = valid;
  zeroOrthoSize.projection = 1U;
  zeroOrthoSize.orthographicSize = 0.0F;

  const CameraEntry *invalid[] = {
      &nanPosition, &infTarget,     &nanUp,  &nanFov,        &zeroNear,
      &negativeNear, &farInsideNear, &infFar, &negativeBlend, &zeroOrthoSize};
  for (const CameraEntry *entry : invalid) {
    if (cm.push_camera(kOwnerB, *entry, 1.0F) ||
        cm.push_camera(kOwnerA, *entry, 1.0F)) {
      return false;
    }
    const CameraEntry *live = cm.active_camera();
    if ((cm.camera_count() != 1U) || (live == nullptr) ||
        !nearly(live->position.x, 3.0F) ||
        !nearly(live->nearPlane, valid.nearPlane) ||
        !nearly(live->farPlane, valid.farPlane)) {
      return false;
    }
  }

  // A NaN priority is refused too.
  if (cm.push_camera(kOwnerB, valid, kNan)) {
    return false;
  }

  // Invalid shakes: refused with no slot activated, and the evaluated
  // camera stays exactly the baseline.
  const float badShakes[][4] = {
      {kNan, 15.0F, 1.0F, 1.0F},  {0.1F, kInf, 1.0F, 1.0F},
      {0.1F, 15.0F, 0.0F, 1.0F},  {0.1F, 15.0F, -1.0F, 1.0F},
      {-0.1F, 15.0F, 1.0F, 1.0F}, {0.1F, -1.0F, 1.0F, 1.0F},
      {0.1F, 15.0F, 1.0F, kNan},  {0.1F, 15.0F, 1.0F, -1.0F}};
  for (const auto &shake : badShakes) {
    if (cm.add_shake(shake[0], shake[1], shake[2], shake[3])) {
      return false;
    }
  }

  math::Vec3 position{};
  math::Vec3 target{};
  math::Vec3 up{};
  float fov = 0.0F;
  float nearPlane = 0.0F;
  float farPlane = 0.0F;
  cm.evaluate(10.0F, &position, &target, &up, &fov, &nearPlane, &farPlane);
  if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
      !std::isfinite(position.z) || !std::isfinite(target.x) ||
      !std::isfinite(fov) || !nearly(position.x, 3.0F) ||
      !nearly(position.y, 2.0F) || !nearly(position.z, 1.0F)) {
    return false;
  }

  // Boundary-valid values stay accepted: a tiny positive near with far
  // just beyond it, a zero blend speed, and an all-zero (but positive
  // duration) shake.
  CameraEntry boundary = valid;
  boundary.nearPlane = 1.0e-6F;
  boundary.farPlane = 2.0e-6F;
  boundary.blendSpeed = 0.0F;
  if (!cm.push_camera(kOwnerB, boundary, 2.0F)) {
    return false;
  }
  if (!cm.add_shake(0.0F, 0.0F, 0.001F, 0.0F)) {
    return false;
  }
  return true;
}

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
         nearly(active->target.z, 3.0F) && nearly(active->position.x, 1.0F) &&
         nearly(active->position.y, 4.0F) && nearly(active->position.z, 11.0F);
}

/// Rotated + scaled owner: the pivot offset must scale then rotate with the
/// entity's world transform, matching child-transform composition.
bool test_spring_arm_composes_rotation_and_scale() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Transform transform{};
  transform.position = math::Vec3(1.0F, 2.0F, 3.0F);
  transform.rotation = math::Quat(0.0F, 1.0F, 0.0F, 0.0F);
  transform.scale = math::Vec3(2.0F, 2.0F, 2.0F);
  const Entity entity = world->create_scene_object(transform);
  if (entity == kInvalidEntity) {
    return false;
  }

  SpringArmComponent arm{};
  arm.armLength = 8.0F;
  arm.currentLength = 8.0F;
  arm.offset = math::Vec3(1.0F, 0.0F, 0.0F);
  arm.lagSpeed = 100.0F;
  arm.collisionEnabled = false;
  if (!world->add_spring_arm(entity, arm)) {
    return false;
  }

  update_spring_arm_cameras(*world, 1.0F);
  const CameraEntry *active = world->camera_manager().active_camera();
  if (active == nullptr) {
    return false;
  }

  return nearly(active->target.x, -1.0F) && nearly(active->target.y, 2.0F) &&
         nearly(active->target.z, 3.0F) && nearly(active->position.x, -1.0F) &&
         nearly(active->position.y, 2.0F) && nearly(active->position.z, -5.0F);
}

/// Parented owner: the arm must consume the hierarchy-composed world
/// transform, not the child's local transform.
bool test_spring_arm_uses_parent_composed_transform() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Transform parentLocal{};
  parentLocal.position = math::Vec3(10.0F, 0.0F, 0.0F);
  const Entity parent = world->create_scene_object(parentLocal);
  if (parent == kInvalidEntity) {
    return false;
  }

  Transform childLocal{};
  childLocal.parentId = world->persistent_id(parent);
  const Entity child = world->create_scene_object(childLocal);
  if (child == kInvalidEntity) {
    return false;
  }

  SpringArmComponent arm{};
  arm.armLength = 4.0F;
  arm.currentLength = 4.0F;
  arm.offset = math::Vec3(0.0F, 2.0F, 0.0F);
  arm.lagSpeed = 100.0F;
  arm.collisionEnabled = false;
  if (!world->add_spring_arm(child, arm)) {
    return false;
  }

  world->begin_transform_phase();
  world->end_frame_phase();

  update_spring_arm_cameras(*world, 1.0F);
  const CameraEntry *active = world->camera_manager().active_camera();
  if (active == nullptr) {
    return false;
  }

  return nearly(active->target.x, 10.0F) && nearly(active->target.y, 2.0F) &&
         nearly(active->target.z, 0.0F) && nearly(active->position.x, 10.0F) &&
         nearly(active->position.y, 2.0F) && nearly(active->position.z, 4.0F);
}

/// Collision sweep: a wall between pivot and camera clamps the arm to the
/// hit distance while the owner's own collider is skipped; disabling
/// collision keeps the authored length.
bool test_spring_arm_collision_clamps_length() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  const Entity owner = world->create_scene_object();
  if (owner == kInvalidEntity) {
    return false;
  }
  Collider ownCollider{};
  ownCollider.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_collider(owner, ownCollider)) {
    return false;
  }

  Transform wallLocal{};
  wallLocal.position = math::Vec3(0.0F, 0.0F, 4.0F);
  const Entity wall = world->create_scene_object(wallLocal);
  if (wall == kInvalidEntity) {
    return false;
  }
  Collider wallCollider{};
  wallCollider.halfExtents = math::Vec3(2.0F, 2.0F, 0.5F);
  if (!world->add_collider(wall, wallCollider)) {
    return false;
  }

  SpringArmComponent arm{};
  arm.armLength = 8.0F;
  arm.currentLength = 8.0F;
  arm.offset = math::Vec3(0.0F, 0.0F, 0.0F);
  arm.lagSpeed = 100.0F;
  arm.collisionRadius = 0.25F;
  arm.collisionEnabled = true;
  if (!world->add_spring_arm(owner, arm)) {
    return false;
  }

  update_spring_arm_cameras(*world, 1.0F);
  SpringArmComponent clamped{};
  if (!world->get_spring_arm(owner, &clamped)) {
    return false;
  }
  const CameraEntry *active = world->camera_manager().active_camera();
  if (active == nullptr) {
    return false;
  }
  if (!nearly(clamped.currentLength, 3.25F) ||
      !nearly(active->position.z, 3.25F)) {
    return false;
  }

  SpringArmComponent *armPtr = world->get_spring_arm_ptr(owner);
  if (armPtr == nullptr) {
    return false;
  }
  armPtr->collisionEnabled = false;
  armPtr->currentLength = 8.0F;
  world->camera_manager().clear();

  update_spring_arm_cameras(*world, 1.0F);
  const CameraEntry *uncapped = world->camera_manager().active_camera();
  return (uncapped != nullptr) && nearly(uncapped->position.z, 8.0F);
}

bool test_camera_component_crud() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return false;
  }

  CameraComponent camera{};
  camera.fovRadians = 1.2F;
  camera.priority = 4.0F;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }
  if (!world->has_camera_component(entity)) {
    return false;
  }

  CameraComponent out{};
  if (!world->get_camera_component(entity, &out)) {
    return false;
  }
  if ((out.fovRadians != 1.2F) || (out.priority != 4.0F)) {
    return false;
  }

  if (!world->remove_camera_component(entity)) {
    return false;
  }
  if (world->has_camera_component(entity)) {
    return false;
  }
  return true;
}

/// A CameraComponent with no SpringArm derives its pose from the entity's
/// world transform (-Z forward, +Y up) and publishes into CameraManager.
bool test_camera_component_pose_from_transform() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Transform transform{};
  transform.position = math::Vec3(1.0F, 2.0F, 3.0F);
  transform.rotation = math::Quat(0.0F, 1.0F, 0.0F, 0.0F); // 180 deg yaw
  const Entity entity = world->create_scene_object(transform);
  if (entity == kInvalidEntity) {
    return false;
  }

  CameraComponent camera{};
  camera.priority = 2.0F;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }

  world->begin_transform_phase();
  world->end_frame_phase();
  update_persistent_cameras(*world, 1.0F);

  const CameraEntry *active = world->camera_manager().active_camera();
  if ((active == nullptr) || (active->ownerEntity != entity)) {
    return false;
  }
  // 180-degree yaw rotates local -Z (forward) to world +Z.
  return nearly(active->position.x, 1.0F) && nearly(active->position.y, 2.0F) &&
        nearly(active->position.z, 3.0F) && nearly(active->target.x, 1.0F) &&
        nearly(active->target.y, 2.0F) && nearly(active->target.z, 4.0F);
}

/// An inactive CameraComponent never reaches CameraManager, and flipping it
/// back to active resumes publishing.
bool test_camera_component_disabled_not_pushed() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return false;
  }

  CameraComponent camera{};
  camera.active = false;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }

  update_persistent_cameras(*world, 1.0F);
  if (world->camera_manager().camera_count() != 0U) {
    return false;
  }

  CameraComponent *ptr = world->get_camera_component_ptr(entity);
  if (ptr == nullptr) {
    return false;
  }
  ptr->active = true;
  update_persistent_cameras(*world, 1.0F);
  return world->camera_manager().camera_count() == 1U;
}

/// Destroying the owning entity removes its published camera, matching the
/// existing World-level camera_manager().on_entity_destroyed cleanup.
bool test_camera_component_destroyed_owner_removes_camera() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return false;
  }

  CameraComponent camera{};
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }
  update_persistent_cameras(*world, 1.0F);
  if (world->camera_manager().camera_count() != 1U) {
    return false;
  }

  if (!world->destroy_entity(entity)) {
    return false;
  }
  return world->camera_manager().camera_count() == 0U;
}

/// Two active cameras at equal priority resolve deterministically: the one
/// authored (added) first keeps winning across repeated evaluations, and
/// find_authored_active_camera reports the tie so editor UI can surface it.
bool test_camera_component_priority_ties_deterministic() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  const Entity first = world->create_scene_object();
  const Entity second = world->create_scene_object();
  if ((first == kInvalidEntity) || (second == kInvalidEntity)) {
    return false;
  }

  CameraComponent camera{};
  camera.priority = 1.0F;
  if (!world->add_camera_component(first, camera) ||
      !world->add_camera_component(second, camera)) {
    return false;
  }

  std::uint32_t tieCount = 0U;
  const Entity winnerA = find_authored_active_camera(*world, &tieCount);
  if ((winnerA != first) || (tieCount != 1U)) {
    return false;
  }

  update_persistent_cameras(*world, 1.0F);
  update_persistent_cameras(*world, 1.0F);
  const CameraEntry *active = world->camera_manager().active_camera();
  if ((active == nullptr) || (active->ownerEntity != first)) {
    return false;
  }

  const Entity winnerB = find_authored_active_camera(*world, &tieCount);
  return (winnerB == first) && (tieCount == 1U);
}

/// No active CameraComponent reports kInvalidEntity, not a stale winner.
bool test_camera_component_none_active_reports_invalid() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return false;
  }
  CameraComponent camera{};
  camera.active = false;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }
  std::uint32_t tieCount = 5U;
  const Entity winner = find_authored_active_camera(*world, &tieCount);
  return (winner == kInvalidEntity) && (tieCount == 0U);
}

/// SpringArm + CameraComponent on the same entity is the standard authored
/// third-person rig: the spring arm's collision-aware boom supplies
/// position/target while the CameraComponent's fov/priority/blendSpeed
/// (not the spring arm's hardcoded defaults) reach CameraManager, and
/// update_persistent_cameras must not also push a second, conflicting entry
/// for the same owner.
bool test_camera_component_with_spring_arm_uses_arm_pose_and_camera_lens() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Transform transform{};
  transform.position = math::Vec3(1.0F, 2.0F, 3.0F);
  const Entity entity = world->create_scene_object(transform);
  if (entity == kInvalidEntity) {
    return false;
  }

  SpringArmComponent arm{};
  arm.armLength = 8.0F;
  arm.currentLength = 8.0F;
  arm.offset = math::Vec3(0.0F, 2.0F, 0.0F);
  arm.lagSpeed = 100.0F;
  arm.collisionEnabled = false;
  if (!world->add_spring_arm(entity, arm)) {
    return false;
  }

  CameraComponent camera{};
  camera.fovRadians = 0.7F;
  camera.priority = 42.0F;
  camera.blendSpeed = 9.0F;
  camera.projection =
      static_cast<std::uint32_t>(CameraProjection::Orthographic);
  camera.orthographicSize = 12.5F;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }

  update_spring_arm_cameras(*world, 1.0F);
  update_persistent_cameras(*world, 1.0F);

  if (world->camera_manager().camera_count() != 1U) {
    return false; // update_persistent_cameras must have skipped this owner.
  }
  const CameraEntry *active = world->camera_manager().active_camera();
  if ((active == nullptr) || (active->ownerEntity != entity)) {
    return false;
  }
  // Spring-arm-derived pose (matches test_spring_arm_updates_camera_position).
  if (!nearly(active->target.x, 1.0F) || !nearly(active->target.y, 4.0F) ||
      !nearly(active->target.z, 3.0F) || !nearly(active->position.x, 1.0F) ||
      !nearly(active->position.y, 4.0F) || !nearly(active->position.z, 11.0F)) {
    return false;
  }
  // CameraComponent-derived lens/priority/blend, including the projection
  // kind and orthographic half-height (#221).
  return nearly(active->fovRadians, 0.7F) && (active->priority == 42.0F) &&
        (active->blendSpeed == 9.0F) &&
        (active->projection ==
         static_cast<std::uint32_t>(CameraProjection::Orthographic)) &&
        nearly(active->orthographicSize, 12.5F);
}

/// A disabled CameraComponent co-located with a SpringArmComponent suppresses
/// the push entirely (the author turned this rig's camera off).
bool test_camera_component_disabled_suppresses_spring_arm_push() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
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
  camera.active = false;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }

  update_spring_arm_cameras(*world, 1.0F);
  return world->camera_manager().camera_count() == 0U;
}

/// camera_component_pose (the editor viewport frustum gizmo's pose query)
/// derives the identical pose update_persistent_cameras would publish,
/// without touching CameraManager, and fails cleanly for an entity with no
/// CameraComponent.
bool test_camera_component_pose_query_matches_manager_push() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  Transform transform{};
  transform.position = math::Vec3(2.0F, 0.0F, -4.0F);
  const Entity entity = world->create_scene_object(transform);
  if (entity == kInvalidEntity) {
    return false;
  }
  CameraComponent camera{};
  camera.fovRadians = 0.5F;
  camera.nearPlane = 0.2F;
  camera.farPlane = 50.0F;
  if (!world->add_camera_component(entity, camera)) {
    return false;
  }

  world->begin_transform_phase();
  world->end_frame_phase();

  renderer::CameraState pose{};
  if (!camera_component_pose(*world, entity, &pose)) {
    return false;
  }

  update_persistent_cameras(*world, 1.0F);
  const CameraEntry *active = world->camera_manager().active_camera();
  if (active == nullptr) {
    return false;
  }
  if (!nearly(pose.position.x, active->position.x) ||
      !nearly(pose.position.y, active->position.y) ||
      !nearly(pose.position.z, active->position.z) ||
      !nearly(pose.target.x, active->target.x) ||
      !nearly(pose.target.y, active->target.y) ||
      !nearly(pose.target.z, active->target.z) ||
      (pose.fovRadians != active->fovRadians) ||
      (pose.nearPlane != active->nearPlane) ||
      (pose.farPlane != active->farPlane)) {
    return false;
  }

  const Entity bare = world->create_entity();
  renderer::CameraState unused{};
  return !camera_component_pose(*world, bare, &unused);
}

/// Scene save/load round-trips a CameraComponent through the production
/// serializer, and the reloaded component drives the pipeline's camera
/// update exactly like a freshly-authored one.
bool test_camera_component_survives_scene_reload() noexcept {
  constexpr PersistentId kCameraPersistentId = 9101U;
  std::unique_ptr<World> source(new (std::nothrow) World());
  Transform transform{};
  transform.position = math::Vec3(5.0F, 0.0F, 0.0F);
  const Entity entity = source->create_scene_object_with_persistent_id(
      kCameraPersistentId, transform);
  if (entity == kInvalidEntity) {
    return false;
  }
  CameraComponent camera{};
  camera.priority = 7.0F;
  camera.fovRadians = 1.1F;
  if (!source->add_camera_component(entity, camera)) {
    return false;
  }

  static char buffer[64U * 1024U];
  std::size_t written = 0U;
  if (!save_scene(*source, buffer, sizeof(buffer), &written)) {
    return false;
  }

  std::unique_ptr<World> loaded(new (std::nothrow) World());
  if (!load_scene(*loaded, buffer, written)) {
    return false;
  }

  const Entity reloadedEntity =
      loaded->find_entity_by_persistent_id(kCameraPersistentId);
  CameraComponent reloadedCamera{};
  if (!loaded->get_camera_component(reloadedEntity, &reloadedCamera)) {
    return false;
  }
  if ((reloadedCamera.priority != 7.0F) ||
      (reloadedCamera.fovRadians != 1.1F)) {
    return false;
  }

  loaded->begin_transform_phase();
  loaded->end_frame_phase();
  update_persistent_cameras(*loaded, 1.0F);
  const CameraEntry *active = loaded->camera_manager().active_camera();
  return (active != nullptr) && (active->ownerEntity == reloadedEntity) &&
        nearly(active->position.x, 5.0F);
}

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
/// #221: the struct evaluate carries the projection kind and half-height;
/// the kind snaps to the winning camera instantly while the size lerps.
bool test_evaluate_carries_projection_kind_and_size() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  CameraManager &mgr = world->camera_manager();

  const Entity ortho = world->create_scene_object();
  CameraEntry orthoEntry{};
  orthoEntry.projection =
      static_cast<std::uint32_t>(CameraProjection::Orthographic);
  orthoEntry.orthographicSize = 20.0F;
  orthoEntry.blendSpeed = 0.5F;
  if (!mgr.push_camera(ortho, orthoEntry, 5.0F)) {
    return false;
  }

  CameraEntry evaluated{};
  mgr.evaluate(0.1F, &evaluated);
  if (evaluated.projection !=
      static_cast<std::uint32_t>(CameraProjection::Orthographic)) {
    return false; // the kind must apply on the first evaluate, not blend in
  }
  if (!nearly(evaluated.orthographicSize, 20.0F)) {
    return false; // first evaluate seeds the blend state from the winner
  }

  // A higher-priority perspective camera takes over: the kind snaps on the
  // next evaluate even though the continuous lens values are still blending.
  const Entity persp = world->create_scene_object();
  CameraEntry perspEntry{};
  perspEntry.projection =
      static_cast<std::uint32_t>(CameraProjection::Perspective);
  perspEntry.orthographicSize = 4.0F;
  perspEntry.blendSpeed = 0.5F;
  if (!mgr.push_camera(persp, perspEntry, 9.0F)) {
    return false;
  }
  mgr.evaluate(0.1F, &evaluated);
  if (evaluated.projection !=
      static_cast<std::uint32_t>(CameraProjection::Perspective)) {
    return false;
  }
  // The half-height is mid-blend: strictly between the two authored values.
  return (evaluated.orthographicSize < 20.0F) &&
         (evaluated.orthographicSize > 4.0F);
}

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
  run("test_camera_shake_large_phase_is_finite_and_deterministic",
      test_camera_shake_large_phase_is_finite_and_deterministic);
  run("test_manager_rejects_invalid_parameters",
      test_manager_rejects_invalid_parameters);
  run("test_spring_arm_crud", test_spring_arm_crud);
  run("test_spring_arm_updates_camera_position",
      test_spring_arm_updates_camera_position);
  run("test_spring_arm_composes_rotation_and_scale",
      test_spring_arm_composes_rotation_and_scale);
  run("test_spring_arm_uses_parent_composed_transform",
      test_spring_arm_uses_parent_composed_transform);
  run("test_spring_arm_collision_clamps_length",
      test_spring_arm_collision_clamps_length);
  run("test_destroyed_owner_removes_camera",
      test_destroyed_owner_removes_camera);
  run("test_camera_component_crud", test_camera_component_crud);
  run("test_camera_component_pose_from_transform",
      test_camera_component_pose_from_transform);
  run("test_camera_component_disabled_not_pushed",
      test_camera_component_disabled_not_pushed);
  run("test_camera_component_destroyed_owner_removes_camera",
      test_camera_component_destroyed_owner_removes_camera);
  run("test_camera_component_priority_ties_deterministic",
      test_camera_component_priority_ties_deterministic);
  run("test_camera_component_none_active_reports_invalid",
      test_camera_component_none_active_reports_invalid);
  run("test_camera_component_with_spring_arm_uses_arm_pose_and_camera_lens",
      test_camera_component_with_spring_arm_uses_arm_pose_and_camera_lens);
  run("test_evaluate_carries_projection_kind_and_size",
      test_evaluate_carries_projection_kind_and_size);
  run("test_camera_component_disabled_suppresses_spring_arm_push",
      test_camera_component_disabled_suppresses_spring_arm_push);
  run("test_camera_component_survives_scene_reload",
      test_camera_component_survives_scene_reload);
  run("test_camera_component_pose_query_matches_manager_push",
      test_camera_component_pose_query_matches_manager_push);
  run("test_clear", test_clear);

  if (failures > 0) {
    std::printf("\n%d test(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nAll camera tests passed.\n");
  return 0;
}
