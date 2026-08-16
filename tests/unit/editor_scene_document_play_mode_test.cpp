// Verifies scene-document identity/dirty state interacts correctly with
// Play mode (issue #158): entering Play with unsaved changes must not
// lose them, document identity (path, display name, dirty status, and
// undo-history position) survives a Play/Stop cycle unchanged, and
// document actions (New/Open/Save) are gated off while Playing so a mid-
// play scene switch can never corrupt the Play/Stop snapshot invariant.

#include "editor_commands.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "engine/editor/editor.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

using namespace engine::editor;
using namespace engine::runtime;

bool scratch_root(char *out, std::size_t capacity) noexcept {
  std::error_code ec{};
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(
      std::filesystem::path("assets/engine_scene_document_play_mode_test"),
      ec);
  if (ec) {
    return false;
  }
  const std::string asString = resolved.string();
  const int written = std::snprintf(out, capacity, "%s", asString.c_str());
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

bool ensure_scratch_root() noexcept {
  char root[900] = {};
  if (!scratch_root(root, sizeof(root))) {
    return false;
  }
  std::error_code ec{};
  std::filesystem::create_directories(std::filesystem::path(root), ec);
  return !ec;
}

bool make_scratch_path(const char *leaf, char *out,
                       std::size_t capacity) noexcept {
  char root[900] = {};
  if (!scratch_root(root, sizeof(root))) {
    return false;
  }
  const int written = std::snprintf(out, capacity, "%s/%s", root, leaf);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

Entity add_named_entity(World &world, const char *name) noexcept {
  const Entity entity = world.create_scene_object();
  if (entity == kInvalidEntity) {
    return kInvalidEntity;
  }
  NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  if (!world.add_name_component(entity, nameComponent)) {
    return kInvalidEntity;
  }
  return entity;
}

bool push_transform_edit(World &world, Entity entity) noexcept {
  auto *command = new (std::nothrow) TransformEditCommand();
  if (command == nullptr) {
    return false;
  }
  command->entity = entity;
  command->persistentId = world.persistent_id(entity);
  command->oldTransform = Transform{};
  command->newTransform.position = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  return editor_session().commandHistory.execute(command);
}

/// EXPECTATION: a titled document's identity, dirty status, and undo
/// position are byte-for-byte unchanged across a Play/Stop cycle.
int check_titled_document_identity_survives_play_stop() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  if (!make_scratch_path("titled.json", scenePath, sizeof(scenePath))) {
    return 2;
  }
  static_cast<void>(std::remove(scenePath));

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 3;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Survivor");
  if ((entity == kInvalidEntity) || !perform_scene_save_as(scenePath) ||
      !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 4;
  }
  if (!scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 5;
  }

  char pathBefore[512] = {};
  std::snprintf(pathBefore, sizeof(pathBefore), "%s", scene_document_path());
  char nameBefore[128] = {};
  std::snprintf(nameBefore, sizeof(nameBefore), "%s",
               scene_document_display_name());
  const std::uint64_t tokenBefore =
      editor_session().commandHistory.current_token();

  start_play_mode();
  if (editor_session().playState != PlayState::Playing) {
    editor_set_world(nullptr);
    return 6;
  }
  // Entering Play must not silently save or discard the unsaved edit.
  if (!scene_document_is_dirty() ||
      (std::strcmp(scene_document_path(), pathBefore) != 0) ||
      (editor_session().commandHistory.current_token() != tokenBefore)) {
    editor_set_world(nullptr);
    return 7;
  }

  stop_play_mode();
  if (editor_session().worldRestoreFailed) {
    editor_set_world(nullptr);
    return 8;
  }

  const bool ok =
      (std::strcmp(scene_document_path(), pathBefore) == 0) &&
      (std::strcmp(scene_document_display_name(), nameBefore) == 0) &&
      scene_document_is_dirty() &&
      (editor_session().commandHistory.current_token() == tokenBefore) &&
      (world->find_entity_by_name("Survivor") != kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 9;
}

/// EXPECTATION: an untitled document's unsaved entities survive a
/// Play/Stop cycle exactly as authored (entering Play never forces a
/// save, a discard, or otherwise loses the work).
int check_untitled_unsaved_changes_survive_play_stop() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "NeverSaved");
  if ((entity == kInvalidEntity) || !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 2;
  }
  if (scene_document_has_path() || !scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 3;
  }

  start_play_mode();
  stop_play_mode();

  const bool ok = !editor_session().worldRestoreFailed &&
                  !scene_document_has_path() && scene_document_is_dirty() &&
                  (std::strcmp(scene_document_display_name(),
                              "Untitled Scene") == 0) &&
                  (world->find_entity_by_name("NeverSaved") != kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 4;
}

/// EXPECTATION: document actions are gated off while Playing/Paused so a
/// mid-play scene switch can never invalidate the Play/Stop snapshot
/// (world_is_editable() requires Stopped; perform_scene_* and the
/// request_scene_* gated entry points all route through it).
int check_document_actions_blocked_while_playing() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  char otherPath[512] = {};
  if (!make_scratch_path("blocked_current.json", scenePath,
                         sizeof(scenePath)) ||
      !make_scratch_path("blocked_other.json", otherPath,
                         sizeof(otherPath))) {
    return 2;
  }
  static_cast<void>(std::remove(scenePath));

  std::unique_ptr<World> otherWriter(new (std::nothrow) World());
  if ((otherWriter == nullptr) ||
      (add_named_entity(*otherWriter, "Other") == kInvalidEntity) ||
      !save_scene(*otherWriter, otherPath)) {
    return 3;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 4;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Original");
  if ((entity == kInvalidEntity) || !perform_scene_save_as(scenePath)) {
    editor_set_world(nullptr);
    return 5;
  }

  start_play_mode();
  if (editor_session().playState != PlayState::Playing) {
    editor_set_world(nullptr);
    return 6;
  }

  if (perform_scene_new() || perform_scene_open(otherPath) ||
      perform_scene_save()) {
    editor_set_world(nullptr);
    return 7; // every direct document action must refuse while Playing
  }

  request_scene_new();
  request_scene_open(otherPath);
  if (scene_document_prompt_open() ||
      (std::strcmp(scene_document_path(), scenePath) != 0) ||
      (world->find_entity_by_name("Original") == kInvalidEntity)) {
    editor_set_world(nullptr);
    return 8; // gated entry points must no-op too, not silently queue
  }

  stop_play_mode();
  const bool ok = !editor_session().worldRestoreFailed &&
                  (std::strcmp(scene_document_path(), scenePath) == 0) &&
                  (world->find_entity_by_name("Original") != kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 9;
}

} // namespace

/// Runs this executable or test program.
int main() {
  struct NamedCheck {
    const char *name;
    int (*fn)();
  };
  const NamedCheck checks[] = {
      {"check_titled_document_identity_survives_play_stop",
       &check_titled_document_identity_survives_play_stop},
      {"check_untitled_unsaved_changes_survive_play_stop",
       &check_untitled_unsaved_changes_survive_play_stop},
      {"check_document_actions_blocked_while_playing",
       &check_document_actions_blocked_while_playing},
  };

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr,
                   "editor_scene_document_play_mode_test: %s failed: %d\n",
                   check.name, result);
      return result;
    }
  }

  std::printf("editor_scene_document_play_mode_test: all tests passed\n");
  return 0;
}
