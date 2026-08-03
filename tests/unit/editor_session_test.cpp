// Verifies editor session world-transition safety (audit C-04): rebinding
// the editor to a different world must reset play state, selection, and
// the Play snapshot, and a snapshot captured from one world must never be
// restored into another even when session state is forced onto the bug's
// historical path.

#include "editor_commands.h"
#include "editor_session.h"
#include "engine/editor/editor.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

/// Names an entity so cross-world contamination is observable; returns
/// the created entity (kInvalidEntity on failure).
engine::runtime::Entity add_named_entity(engine::runtime::World &world,
                                         const char *name) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return engine::runtime::kInvalidEntity;
  }
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  if (!world.add_name_component(entity, nameComponent)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

/// EXPECTATION: switching the bound world mid-play resets play state,
/// selection, step requests, and discards the Play snapshot.
int check_world_switch_resets_session() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> worldA(new (std::nothrow) World());
  std::unique_ptr<World> worldB(new (std::nothrow) World());
  if ((worldA == nullptr) || (worldB == nullptr)) {
    return 1;
  }

  editor_set_world(worldA.get());
  const Entity onlyInA = add_named_entity(*worldA, "OnlyInA");
  if (onlyInA == kInvalidEntity) {
    editor_set_world(nullptr);
    return 2;
  }
  select_entity(onlyInA, false);
  if (!capture_play_snapshot()) {
    editor_set_world(nullptr);
    return 3;
  }
  editor_session().playState = PlayState::Playing;
  editor_session().stepRequested = true;

  editor_set_world(worldB.get());
  const EditorSession &session = editor_session();
  if ((session.playState != PlayState::Stopped) || session.hasPlaySnapshot ||
      (session.playSnapshotWorld != nullptr) ||
      (session.selectedEntity != kInvalidEntity) ||
      (session.selectedEntityCount != 0U) || session.stepRequested ||
      session.worldRestoreFailed) {
    editor_set_world(nullptr);
    return 4;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: even when the session is forced onto the historical bug
/// path (world swapped without editor_set_world), Stop must refuse to
/// restore world A's snapshot into world B and must fail loudly.
int check_snapshot_never_restores_into_other_world() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> worldA(new (std::nothrow) World());
  std::unique_ptr<World> worldB(new (std::nothrow) World());
  if ((worldA == nullptr) || (worldB == nullptr)) {
    return 10;
  }
  if (add_named_entity(*worldB, "OnlyInB") == kInvalidEntity) {
    return 11;
  }

  editor_set_world(worldA.get());
  if (add_named_entity(*worldA, "OnlyInA") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 12;
  }
  if (!capture_play_snapshot()) {
    editor_set_world(nullptr);
    return 13;
  }
  editor_session().playState = PlayState::Playing;

  editor_session().world = worldB.get();
  stop_play_mode();

  if (worldB->find_entity_by_name("OnlyInB") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 14;
  }
  if (worldB->find_entity_by_name("OnlyInA") != kInvalidEntity) {
    editor_set_world(nullptr);
    return 15;
  }
  if (!editor_session().worldRestoreFailed ||
      editor_session().hasPlaySnapshot) {
    editor_set_world(nullptr);
    return 16;
  }
  if (worldA->find_entity_by_name("OnlyInA") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 17;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: a malformed same-world snapshot must not destroy the live
/// world on Stop — scene loading is transactional, so the play world and
/// the snapshot survive, and the failure is surfaced loudly.
int check_malformed_snapshot_preserves_world() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 20;
  }

  editor_set_world(world.get());
  const Entity survivor = add_named_entity(*world, "Survivor");
  if (survivor == kInvalidEntity) {
    editor_set_world(nullptr);
    return 21;
  }
  if (!capture_play_snapshot()) {
    editor_set_world(nullptr);
    return 22;
  }
  editor_session().playState = PlayState::Playing;
  select_entity(survivor, false);

  std::memcpy(editor_session().playSnapshotBuffer.get(), "garbage!", 8U);

  stop_play_mode();

  if (world->find_entity_by_name("Survivor") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 23;
  }
  if (!editor_session().worldRestoreFailed ||
      !editor_session().hasPlaySnapshot ||
      (editor_session().playState != PlayState::Stopped)) {
    editor_set_world(nullptr);
    return 24;
  }
  if ((editor_session().selectedEntity != kInvalidEntity) ||
      (editor_session().selectedEntityCount != 0U)) {
    editor_set_world(nullptr);
    return 25;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: a selection must never alias a different entity that
/// reuses the deleted entity's index slot; dead handles are pruned from
/// the multi-selection and the primary selection.
int check_selection_rejects_recycled_slot() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 30;
  }
  editor_set_world(world.get());

  const Entity keeper = add_named_entity(*world, "Keeper");
  const Entity doomed = add_named_entity(*world, "Doomed");
  if ((keeper == kInvalidEntity) || (doomed == kInvalidEntity)) {
    editor_set_world(nullptr);
    return 31;
  }

  select_entity(keeper, false);
  select_entity(doomed, true);
  if (!world->destroy_entity(doomed)) {
    editor_set_world(nullptr);
    return 32;
  }

  const Entity recycled = add_named_entity(*world, "Recycled");
  if ((recycled == kInvalidEntity) || (recycled.index != doomed.index) ||
      (recycled.generation == doomed.generation)) {
    editor_set_world(nullptr);
    return 33;
  }

  if (is_entity_selected(recycled) || is_entity_selected(doomed)) {
    editor_set_world(nullptr);
    return 34;
  }

  prune_entity_selection();
  if ((editor_session().selectedEntityCount != 1U) ||
      !is_entity_selected(keeper) || (selected_entity() != keeper)) {
    editor_set_world(nullptr);
    return 35;
  }

  select_entity(doomed, false);
  if (selected_entity() != kInvalidEntity) {
    editor_set_world(nullptr);
    return 36;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: a scene load replaces the world's contents (resetting
/// entity generations), so a retained selection must be invalidated by
/// the content-epoch check even when index+generation happen to match a
/// freshly loaded entity.
int check_selection_invalidated_by_scene_load() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 40;
  }
  editor_set_world(world.get());

  const Entity picked = add_named_entity(*world, "Picked");
  if (picked == kInvalidEntity) {
    editor_set_world(nullptr);
    return 41;
  }
  select_entity(picked, false);

  char sceneBuffer[16384] = {};
  std::size_t sceneSize = 0U;
  if (!save_scene(*world, sceneBuffer, sizeof(sceneBuffer), &sceneSize)) {
    editor_set_world(nullptr);
    return 42;
  }
  if (!load_scene(*world, sceneBuffer, sceneSize)) {
    editor_set_world(nullptr);
    return 43;
  }

  const Entity reloaded = world->find_entity_by_name("Picked");
  if (reloaded == kInvalidEntity) {
    editor_set_world(nullptr);
    return 44;
  }
  if (is_entity_selected(reloaded) || is_entity_selected(picked) ||
      (selected_entity() != kInvalidEntity) ||
      (editor_session().selectedEntityCount != 0U)) {
    editor_set_world(nullptr);
    return 45;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: undo/redo must be inert while the world is not editable
/// (during play), and work again once stopped.
int check_history_gated_while_playing() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 50;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Gated");
  if (entity == kInvalidEntity) {
    editor_set_world(nullptr);
    return 51;
  }

  auto *command = new (std::nothrow) TransformEditCommand();
  if (command == nullptr) {
    editor_set_world(nullptr);
    return 52;
  }
  command->entity = entity;
  command->persistentId = world->persistent_id(entity);
  command->oldTransform = Transform{};
  command->newTransform.position = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  editor_session().commandHistory.execute(command);

  editor_session().playState = PlayState::Playing;
  editor_history_undo();
  Transform duringPlay{};
  if (!world->get_transform(entity, &duringPlay) ||
      (duringPlay.position.x != 5.0F) ||
      !editor_session().commandHistory.can_undo()) {
    editor_set_world(nullptr);
    return 53;
  }

  editor_session().playState = PlayState::Stopped;
  editor_history_undo();
  Transform afterStop{};
  if (!world->get_transform(entity, &afterStop) ||
      (afterStop.position.x != 0.0F)) {
    editor_set_world(nullptr);
    return 54;
  }

  editor_session().playState = PlayState::Paused;
  editor_history_redo();
  Transform duringPause{};
  if (!world->get_transform(entity, &duringPause) ||
      (duringPause.position.x != 0.0F)) {
    editor_set_world(nullptr);
    return 55;
  }

  editor_session().playState = PlayState::Stopped;
  editor_history_redo();
  Transform afterRedo{};
  if (!world->get_transform(entity, &afterRedo) ||
      (afterRedo.position.x != 5.0F)) {
    editor_set_world(nullptr);
    return 56;
  }

  editor_session().commandHistory.clear();
  editor_set_world(nullptr);
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_world_switch_resets_session();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_snapshot_never_restores_into_other_world();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_malformed_snapshot_preserves_world();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_selection_rejects_recycled_slot();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_selection_invalidated_by_scene_load();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_history_gated_while_playing();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  std::printf("editor_session_test: all tests passed\n");
  return 0;
}
