// Verifies scene-document identity/dirty state interacts correctly with
// Play mode (issue #158): entering Play with unsaved changes must not
// lose them, document identity (path, display name, dirty status, and
// undo-history position) survives a Play/Stop cycle unchanged, and
// document actions (New/Open/Save) are gated off while Playing so a mid-
// play scene switch can never corrupt the Play/Stop snapshot invariant.
// After a failed Stop restore the menu's own entry points (New, Open,
// Save, Save As, and the unsaved-changes prompt) are the recovery path
// and must work.

#include "editor_commands.h"
#include "editor_scene_document.h"
#include "editor_scene_document_fixture.h"
#include "editor_session.h"
#include "engine/core/platform.h"
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
  // No pipeline frame runs here, so finish the Stop as the pipeline
  // would after the end hooks.
  finish_play_stop();
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
  finish_play_stop();

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
  finish_play_stop();
  const bool ok = !editor_session().worldRestoreFailed &&
                  (std::strcmp(scene_document_path(), scenePath) == 0) &&
                  (world->find_entity_by_name("Original") != kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 9;
}

/// Plays, corrupts the pre-play snapshot and stops, so the restore fails
/// and the latch is set with the play world preserved. True when latched.
bool latch_by_failed_restore() noexcept {
  start_play_mode();
  if ((editor_session().playState != PlayState::Playing) ||
      !editor_session().hasPlaySnapshot) {
    return false;
  }
  std::memcpy(editor_session().playSnapshotBuffer.get(), "garbage!", 8U);
  stop_play_mode();
  finish_play_stop();
  return editor_session().worldRestoreFailed &&
         (editor_session().playState == PlayState::Stopped);
}

/// Answers the Save As or Open dialog the session is waiting on with
/// `path` and applies the result, as the next frame would.
bool answer_pending_dialog(const char *path) noexcept {
  const engine::core::FileDialogTicket ticket =
      editor_session().document.activeDialog;
  if ((ticket == engine::core::kNoFileDialog) ||
      !engine::core::platform_answer_scripted_file_dialog(ticket, path)) {
    return false;
  }
  scene_document_poll_dialog_result();
  return editor_session().document.activeDialog == engine::core::kNoFileDialog;
}

bool file_holds_entity(const char *path, const char *name) noexcept {
  std::unique_ptr<World> reader(new (std::nothrow) World());
  return (reader != nullptr) && load_scene(*reader, path) &&
         (reader->find_entity_by_name(name) != kInvalidEntity);
}

/// EXPECTATION: after a failed Stop restore the File menu's entry
/// points recover the editor. Save refuses in place and says why; New and
/// Open ask first, since the preserved world is unsaved, and the prompt's
/// Save exports it through Save As; Save As alone exports it and ends the
/// recovery. On base every request_* entry point returned silently on the
/// latch, so none of this happened.
int check_recovery_entry_points_after_failed_restore() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  char otherPath[512] = {};
  char exportPath[512] = {};
  char secondExportPath[512] = {};
  if (!make_scratch_path("recover_current.json", scenePath,
                         sizeof(scenePath)) ||
      !make_scratch_path("recover_other.json", otherPath, sizeof(otherPath)) ||
      !make_scratch_path("recover_export.json", exportPath,
                         sizeof(exportPath)) ||
      !make_scratch_path("recover_export_2.json", secondExportPath,
                         sizeof(secondExportPath))) {
    return 2;
  }
  static_cast<void>(std::remove(scenePath));
  static_cast<void>(std::remove(exportPath));
  static_cast<void>(std::remove(secondExportPath));

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
  engine::core::platform_set_scripted_file_dialogs(true);
  const auto fail = [](int code) noexcept {
    engine::core::platform_set_scripted_file_dialogs(false);
    editor_set_world(nullptr);
    return code;
  };

  if ((add_named_entity(*world, "Survivor") == kInvalidEntity) ||
      !perform_scene_save_as(scenePath) || scene_document_is_dirty()) {
    return fail(5);
  }
  if (!latch_by_failed_restore()) {
    return fail(6);
  }
  // The preserved world is not the file on disk.
  if (!scene_document_is_dirty()) {
    return fail(7);
  }

  // --- Save in place refuses, with its reason, and writes nothing.
  request_save_scene();
  if ((scene_document_last_error()[0] == '\0') ||
      !file_holds_entity(scenePath, "Survivor") ||
      !editor_session().worldRestoreFailed) {
    return fail(8);
  }

  // --- New asks first; the prompt's Save exports through Save As (not
  // in place), then the New proceeds.
  request_scene_new();
  if (!scene_document_prompt_open()) {
    return fail(9);
  }
  scene_document_prompt_choose_save();
  if (editor_session().document.activeDialog == engine::core::kNoFileDialog) {
    return fail(10);
  }
  if (!answer_pending_dialog(exportPath)) {
    return fail(11);
  }
  if (!file_holds_entity(exportPath, "Survivor") ||
      editor_session().worldRestoreFailed || scene_document_prompt_open() ||
      (world->find_entity_by_name("Survivor") != kInvalidEntity) ||
      scene_document_has_path()) {
    return fail(12);
  }

  // --- Open through the dialog asks first too; Discard opens the file
  // and clears the latch.
  if ((add_named_entity(*world, "Second") == kInvalidEntity) ||
      !latch_by_failed_restore()) {
    return fail(13);
  }
  request_open_scene_dialog();
  if (!answer_pending_dialog(otherPath) || !scene_document_prompt_open()) {
    return fail(14);
  }
  scene_document_prompt_choose_discard();
  if (editor_session().worldRestoreFailed ||
      (world->find_entity_by_name("Other") == kInvalidEntity) ||
      (std::strcmp(scene_document_path(), otherPath) != 0) ||
      scene_document_is_dirty()) {
    return fail(15);
  }

  // --- Save As alone exports the preserved world and ends the recovery:
  // the export is the document, clean, with nothing to undo into.
  if ((add_named_entity(*world, "Third") == kInvalidEntity) ||
      !latch_by_failed_restore()) {
    return fail(16);
  }
  request_save_scene_as();
  if (!answer_pending_dialog(secondExportPath)) {
    return fail(17);
  }
  if (!file_holds_entity(secondExportPath, "Third") ||
      editor_session().worldRestoreFailed ||
      (std::strcmp(scene_document_path(), secondExportPath) != 0) ||
      scene_document_is_dirty() || editor_history_can_undo()) {
    return fail(18);
  }

  // --- While playing, no Open dialog is shown at all.
  start_play_mode();
  request_open_scene_dialog();
  const bool dialogWhilePlaying =
      editor_session().document.activeDialog != engine::core::kNoFileDialog;
  stop_play_mode();
  finish_play_stop();
  if (dialogWhilePlaying || editor_session().worldRestoreFailed) {
    return fail(19);
  }

  engine::core::platform_set_scripted_file_dialogs(false);
  editor_set_world(nullptr);
  return 0;
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
      {"check_recovery_entry_points_after_failed_restore",
       &check_recovery_entry_points_after_failed_restore},
  };

  // Saves below add to the recent-scenes list; the guard keeps that out
  // of the developer's real save directory.
  engine::tests::RecentScenesGuard recentGuard;
  char recentScratch[1000] = {};
  if (!ensure_scratch_root() ||
      !make_scratch_path("recent_scenes_fixture", recentScratch,
                         sizeof(recentScratch)) ||
      !recentGuard.arm(recentScratch)) {
    std::fprintf(stderr, "editor_scene_document_play_mode_test: the "
                         "recent-scenes guard could not be armed\n");
    return 98;
  }

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr,
                   "editor_scene_document_play_mode_test: %s failed: %d\n",
                   check.name, result);
      static_cast<void>(recentGuard.disarm());
      return result;
    }
  }

  if (!recentGuard.disarm()) {
    return 99;
  }
  std::printf("editor_scene_document_play_mode_test: all tests passed\n");
  return 0;
}
