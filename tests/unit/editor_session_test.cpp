// Verifies editor session world-transition safety (audit C-04): rebinding
// the editor to a different world must reset play state, selection, and
// the Play snapshot, and a snapshot captured from one world must never be
// restored into another even when session state is forced onto the bug's
// historical path.

#include "editor_session.h"
#include "engine/editor/editor.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

/// Names an entity so cross-world contamination is observable.
bool add_named_entity(engine::runtime::World &world,
                      const char *name) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  return world.add_name_component(entity, nameComponent);
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
  if (!add_named_entity(*worldA, "OnlyInA")) {
    editor_set_world(nullptr);
    return 2;
  }
  select_entity(1U, false);
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
      (session.selectedEntityIndex != 0U) ||
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
  if (!add_named_entity(*worldB, "OnlyInB")) {
    return 11;
  }

  editor_set_world(worldA.get());
  if (!add_named_entity(*worldA, "OnlyInA")) {
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

  std::printf("editor_session_test: all tests passed\n");
  return 0;
}
