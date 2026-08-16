// Verifies the inspector edit-gesture pipeline (audit M-24): field edits
// route through World validation and the command history as one undoable
// command per gesture, gestures split on target change, invalid values are
// rejected without recording history, a changed animation controller path
// drops the cached controller binding, and edited rotations renormalize.

#include "editor_commands.h"
#include "editor_session.h"
#include "engine/runtime/animation.h"
#include "engine/runtime/world.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace {

using engine::editor::ComponentEditSnapshot;
using engine::editor::ComponentEditType;
using engine::runtime::Entity;
using engine::runtime::World;

/// Binds a fresh world to the editor session; restores on destruction.
struct SessionWorldScope final {
  World *previousWorld = nullptr;

  explicit SessionWorldScope(World *world) noexcept {
    previousWorld = engine::editor::editor_session().world;
    engine::editor::editor_session().world = world;
  }

  ~SessionWorldScope() noexcept {
    engine::editor::inspector_abandon_pending_edit();
    engine::editor::editor_session().commandHistory.clear();
    engine::editor::editor_session().world = previousWorld;
  }
};

/// A multi-step scrub on one component must apply live, land in history
/// as a single command on commit, and round-trip through undo/redo.
int check_gesture_commits_single_undo() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  SessionWorldScope scope(world.get());

  const Entity entity = world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return 2;
  }
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  if (!world->add_rigid_body(entity, body)) {
    return 3;
  }

  ComponentEditSnapshot before{};
  before.rigidBody = body;
  ComponentEditSnapshot step1 = before;
  step1.rigidBody.inverseMass = 2.0F;
  if (!engine::editor::inspector_stage_component_edit(
          entity, ComponentEditType::RigidBody, before, step1)) {
    return 4;
  }
  ComponentEditSnapshot step2 = step1;
  step2.rigidBody.inverseMass = 3.0F;
  if (!engine::editor::inspector_stage_component_edit(
          entity, ComponentEditType::RigidBody, step1, step2)) {
    return 5;
  }
  if (engine::editor::editor_session().commandHistory.can_undo()) {
    return 6;
  }

  engine::runtime::RigidBody live{};
  if (!world->get_rigid_body(entity, &live) || (live.inverseMass != 3.0F)) {
    return 7;
  }

  engine::editor::inspector_commit_pending_edit();
  if (!engine::editor::editor_session().commandHistory.can_undo() ||
      engine::editor::inspector_has_pending_edit()) {
    return 8;
  }

  engine::editor::editor_session().commandHistory.undo();
  engine::runtime::RigidBody reverted{};
  if (!world->get_rigid_body(entity, &reverted) ||
      (reverted.inverseMass != 1.0F) ||
      engine::editor::editor_session().commandHistory.can_undo()) {
    return 9;
  }

  engine::editor::editor_session().commandHistory.redo();
  engine::runtime::RigidBody redone{};
  if (!world->get_rigid_body(entity, &redone) ||
      (redone.inverseMass != 3.0F)) {
    return 10;
  }

  return 0;
}

/// Staging against a different target must commit the previous gesture
/// first, so each entity's edit is its own history entry.
int check_gesture_splits_on_target_change() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 20;
  }
  SessionWorldScope scope(world.get());

  const Entity first = world->create_scene_object();
  const Entity second = world->create_scene_object();
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity)) {
    return 21;
  }
  engine::runtime::MeshComponent mesh{};
  mesh.albedo = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  if (!world->add_mesh_component(first, mesh) ||
      !world->add_mesh_component(second, mesh)) {
    return 22;
  }

  ComponentEditSnapshot before{};
  before.mesh = mesh;
  ComponentEditSnapshot editFirst = before;
  editFirst.mesh.albedo.x = 0.25F;
  if (!engine::editor::inspector_stage_component_edit(
          first, ComponentEditType::Mesh, before, editFirst)) {
    return 23;
  }
  ComponentEditSnapshot editSecond = before;
  editSecond.mesh.albedo.y = 0.5F;
  if (!engine::editor::inspector_stage_component_edit(
          second, ComponentEditType::Mesh, before, editSecond)) {
    return 24;
  }
  engine::editor::inspector_commit_pending_edit();

  engine::editor::editor_session().commandHistory.undo();
  engine::runtime::MeshComponent secondReverted{};
  engine::runtime::MeshComponent firstStillEdited{};
  if (!world->get_mesh_component(second, &secondReverted) ||
      (secondReverted.albedo.y != 1.0F) ||
      !world->get_mesh_component(first, &firstStillEdited) ||
      (firstStillEdited.albedo.x != 0.25F)) {
    return 25;
  }

  engine::editor::editor_session().commandHistory.undo();
  engine::runtime::MeshComponent firstReverted{};
  if (!world->get_mesh_component(first, &firstReverted) ||
      (firstReverted.albedo.x != 1.0F)) {
    return 26;
  }

  return 0;
}

/// A World-rejected value (non-finite inverse mass) must leave the world
/// untouched and never open a gesture or reach the history.
int check_invalid_value_rejected_without_history() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 30;
  }
  SessionWorldScope scope(world.get());

  const Entity entity = world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return 31;
  }
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  if (!world->add_rigid_body(entity, body)) {
    return 32;
  }

  ComponentEditSnapshot before{};
  before.rigidBody = body;
  ComponentEditSnapshot poisoned = before;
  poisoned.rigidBody.inverseMass = std::numeric_limits<float>::quiet_NaN();
  if (engine::editor::inspector_stage_component_edit(
          entity, ComponentEditType::RigidBody, before, poisoned)) {
    return 33;
  }
  if (engine::editor::inspector_has_pending_edit()) {
    return 34;
  }
  engine::editor::inspector_commit_pending_edit();
  if (engine::editor::editor_session().commandHistory.can_undo()) {
    return 35;
  }
  engine::runtime::RigidBody untouched{};
  if (!world->get_rigid_body(entity, &untouched) ||
      (untouched.inverseMass != 1.0F)) {
    return 36;
  }

  return 0;
}

/// Editing the controller path must reset the cached controller slot and
/// state-machine position so the new controller actually loads; undo must
/// restore the original binding with the original path.
int check_animation_path_edit_resets_binding() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 40;
  }
  SessionWorldScope scope(world.get());

  const Entity entity = world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return 41;
  }
  engine::runtime::AnimationComponent animation{};
  std::snprintf(animation.controllerPath, sizeof(animation.controllerPath),
                "%s", "assets/old.controller");
  animation.controllerSlot = 7U;
  animation.currentState = 2U;
  animation.stateTime = 1.5F;
  if (!world->add_animation_component(entity, animation)) {
    return 42;
  }

  ComponentEditSnapshot before{};
  before.animation = animation;
  ComponentEditSnapshot after = before;
  std::snprintf(after.animation.controllerPath,
                sizeof(after.animation.controllerPath), "%s",
                "assets/new.controller");
  if (!engine::editor::inspector_stage_component_edit(
          entity, ComponentEditType::Animation, before, after)) {
    return 43;
  }

  engine::runtime::AnimationComponent rebound{};
  if (!world->get_animation_component(entity, &rebound) ||
      (std::strcmp(rebound.controllerPath, "assets/new.controller") != 0) ||
      (rebound.controllerSlot != engine::runtime::kInvalidAnimSlot) ||
      (rebound.currentState != 0U) || (rebound.stateTime != 0.0F)) {
    return 44;
  }

  engine::editor::inspector_commit_pending_edit();
  engine::editor::editor_session().commandHistory.undo();
  engine::runtime::AnimationComponent restored{};
  if (!world->get_animation_component(entity, &restored) ||
      (std::strcmp(restored.controllerPath, "assets/old.controller") != 0) ||
      (restored.controllerSlot != 7U) || (restored.currentState != 2U)) {
    return 45;
  }

  return 0;
}

/// A speed-only animation edit (path unchanged) must keep the live
/// controller binding intact.
int check_animation_speed_edit_keeps_binding() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 50;
  }
  SessionWorldScope scope(world.get());

  const Entity entity = world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return 51;
  }
  engine::runtime::AnimationComponent animation{};
  std::snprintf(animation.controllerPath, sizeof(animation.controllerPath),
                "%s", "assets/old.controller");
  animation.controllerSlot = 7U;
  if (!world->add_animation_component(entity, animation)) {
    return 52;
  }

  ComponentEditSnapshot before{};
  before.animation = animation;
  ComponentEditSnapshot after = before;
  after.animation.playbackSpeed = 2.0F;
  if (!engine::editor::inspector_stage_component_edit(
          entity, ComponentEditType::Animation, before, after)) {
    return 53;
  }

  engine::runtime::AnimationComponent live{};
  if (!world->get_animation_component(entity, &live) ||
      (live.playbackSpeed != 2.0F) || (live.controllerSlot != 7U)) {
    return 54;
  }

  return 0;
}

/// An edited rotation must reach the world renormalized; a zeroed-out
/// rotation falls back to the pre-edit value instead of poisoning the
/// transform.
int check_transform_rotation_renormalized() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 60;
  }
  SessionWorldScope scope(world.get());

  const Entity entity = world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return 61;
  }

  engine::runtime::Transform transform{};
  if (!world->get_transform(entity, &transform)) {
    return 62;
  }
  ComponentEditSnapshot before{};
  before.transform = transform;
  ComponentEditSnapshot scaledRotation = before;
  scaledRotation.transform.rotation =
      engine::math::Quat{0.0F, 0.0F, 0.0F, 2.0F};
  if (!engine::editor::inspector_stage_component_edit(
          entity, ComponentEditType::Transform, before, scaledRotation)) {
    return 63;
  }
  engine::runtime::Transform normalized{};
  if (!world->get_transform(entity, &normalized) ||
      (std::fabs(normalized.rotation.w - 1.0F) > 1.0e-6F) ||
      (normalized.rotation.x != 0.0F)) {
    return 64;
  }

  ComponentEditSnapshot zeroedRotation = before;
  zeroedRotation.transform.rotation =
      engine::math::Quat{0.0F, 0.0F, 0.0F, 0.0F};
  if (!engine::editor::inspector_stage_component_edit(
          entity, ComponentEditType::Transform, before, zeroedRotation)) {
    return 65;
  }
  engine::runtime::Transform recovered{};
  if (!world->get_transform(entity, &recovered) ||
      (std::fabs(recovered.rotation.w - 1.0F) > 1.0e-6F)) {
    return 66;
  }

  return 0;
}

/// A CameraComponent priority edit round-trips through the full
/// stage/commit/undo/redo pipeline exactly like RigidBody above -- proves
/// the registry-generated ComponentEditType::Camera dispatch (issue #161)
/// reaches actual undo/redo history, not just capture/apply in isolation.
int check_camera_component_undo_redo() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 70;
  }
  SessionWorldScope scope(world.get());

  const Entity entity = world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return 71;
  }
  engine::runtime::CameraComponent camera{};
  camera.priority = 1.0F;
  if (!world->add_camera_component(entity, camera)) {
    return 72;
  }

  ComponentEditSnapshot before{};
  before.camera = camera;
  ComponentEditSnapshot after = before;
  after.camera.priority = 5.0F;
  if (!engine::editor::inspector_stage_component_edit(
          entity, ComponentEditType::Camera, before, after)) {
    return 73;
  }
  engine::editor::inspector_commit_pending_edit();
  if (!engine::editor::editor_session().commandHistory.can_undo()) {
    return 74;
  }

  engine::runtime::CameraComponent live{};
  if (!world->get_camera_component(entity, &live) || (live.priority != 5.0F)) {
    return 75;
  }

  engine::editor::editor_session().commandHistory.undo();
  engine::runtime::CameraComponent reverted{};
  if (!world->get_camera_component(entity, &reverted) ||
      (reverted.priority != 1.0F)) {
    return 76;
  }

  engine::editor::editor_session().commandHistory.redo();
  engine::runtime::CameraComponent redone{};
  if (!world->get_camera_component(entity, &redone) ||
      (redone.priority != 5.0F)) {
    return 77;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_gesture_commits_single_undo();
  if (result != 0) {
    std::fprintf(stderr, "editor_inspector_edit_test failed: %d\n", result);
    return result;
  }

  result = check_gesture_splits_on_target_change();
  if (result != 0) {
    std::fprintf(stderr, "editor_inspector_edit_test failed: %d\n", result);
    return result;
  }

  result = check_invalid_value_rejected_without_history();
  if (result != 0) {
    std::fprintf(stderr, "editor_inspector_edit_test failed: %d\n", result);
    return result;
  }

  result = check_animation_path_edit_resets_binding();
  if (result != 0) {
    std::fprintf(stderr, "editor_inspector_edit_test failed: %d\n", result);
    return result;
  }

  result = check_animation_speed_edit_keeps_binding();
  if (result != 0) {
    std::fprintf(stderr, "editor_inspector_edit_test failed: %d\n", result);
    return result;
  }

  result = check_transform_rotation_renormalized();
  if (result != 0) {
    std::fprintf(stderr, "editor_inspector_edit_test failed: %d\n", result);
    return result;
  }

  result = check_camera_component_undo_redo();
  if (result != 0) {
    std::fprintf(stderr, "editor_inspector_edit_test failed: %d\n", result);
    return result;
  }

  std::printf("editor_inspector_edit_test: all tests passed\n");
  return 0;
}
