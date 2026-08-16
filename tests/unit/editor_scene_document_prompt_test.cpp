// Verifies the unsaved-change confirm-prompt state machine (issue #158):
// New/Open/quit run immediately on a clean document, dirty documents arm
// the Save/Discard/Cancel prompt instead, Cancel leaves everything
// untouched, Discard proceeds without saving, and Save either saves in
// place (titled document) or resolves through the async Save As dialog
// handoff (untitled document) before continuing the deferred action. A
// failed Save keeps the prompt armed instead of silently losing the
// pending action. ImGui draw code is out of scope here (exempt per
// CLAUDE.md); every check below drives the production functions the
// panel calls, not a copy.

#include "editor_commands.h"
#include "editor_scene_document.h"
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

/// Absolute scratch root nested under the real editor asset root so
/// perform_scene_save_as's production jail check accepts these paths.
bool scratch_root(char *out, std::size_t capacity) noexcept {
  std::error_code ec{};
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(
      std::filesystem::path("assets/engine_scene_document_prompt_test"), ec);
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

/// EXPECTATION: New/Open run immediately (no prompt) when the document is
/// already clean.
int check_clean_document_actions_run_immediately() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char openTarget[512] = {};
  if (!make_scratch_path("open_target.json", openTarget, sizeof(openTarget))) {
    return 2;
  }
  static_cast<void>(std::remove(openTarget));

  std::unique_ptr<World> writer(new (std::nothrow) World());
  if ((writer == nullptr) ||
      (add_named_entity(*writer, "FromDisk") == kInvalidEntity) ||
      !save_scene(*writer, openTarget)) {
    return 3;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 4;
  }
  editor_set_world(world.get());

  request_scene_new(); // no-op-ish: already untitled/clean/empty
  if (scene_document_prompt_open()) {
    editor_set_world(nullptr);
    return 5;
  }

  request_scene_open(openTarget);
  const bool ok = !scene_document_prompt_open() && scene_document_has_path() &&
                  (std::strcmp(scene_document_path(), openTarget) == 0) &&
                  (world->find_entity_by_name("FromDisk") != kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 6;
}

/// EXPECTATION: a dirty document arms the prompt instead of performing
/// New immediately; Cancel closes it without touching anything, and the
/// action can be re-armed afterward (proving Cancel leaves no stale
/// pending state behind).
int check_dirty_new_arms_prompt_and_cancel_restores() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Keep");
  if ((entity == kInvalidEntity) || !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 2;
  }

  request_scene_new();
  if (!scene_document_prompt_open() || (world->alive_entity_count() == 0U)) {
    editor_set_world(nullptr);
    return 3;
  }

  scene_document_prompt_choose_cancel();
  if (scene_document_prompt_open() || (world->alive_entity_count() == 0U) ||
      !scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 4;
  }

  // Re-arms cleanly: no leftover pending action from the canceled attempt.
  request_scene_new();
  const bool ok = scene_document_prompt_open();
  scene_document_prompt_choose_cancel();
  editor_set_world(nullptr);
  return ok ? 0 : 5;
}

/// EXPECTATION: Discard proceeds with the pending action without saving.
int check_dirty_new_discard_performs_new() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Discarded");
  if ((entity == kInvalidEntity) || !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 2;
  }

  request_scene_new();
  if (!scene_document_prompt_open()) {
    editor_set_world(nullptr);
    return 3;
  }

  scene_document_prompt_choose_discard();
  const bool ok = !scene_document_prompt_open() &&
                  (world->alive_entity_count() == 0U) &&
                  !scene_document_has_path() && !scene_document_is_dirty();
  editor_set_world(nullptr);
  return ok ? 0 : 4;
}

/// EXPECTATION: choosing Save on a titled, dirty document saves in place
/// (no dialog needed) and then continues the deferred Open.
int check_dirty_open_save_continues_with_titled_document() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char currentPath[512] = {};
  char targetPath[512] = {};
  if (!make_scratch_path("current.json", currentPath, sizeof(currentPath)) ||
      !make_scratch_path("target.json", targetPath, sizeof(targetPath))) {
    return 2;
  }
  static_cast<void>(std::remove(currentPath));
  static_cast<void>(std::remove(targetPath));

  std::unique_ptr<World> targetWriter(new (std::nothrow) World());
  if ((targetWriter == nullptr) ||
      (add_named_entity(*targetWriter, "TargetEntity") == kInvalidEntity) ||
      !save_scene(*targetWriter, targetPath)) {
    return 3;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 4;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "CurrentEntity");
  if ((entity == kInvalidEntity) || !perform_scene_save_as(currentPath) ||
      !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 5;
  }
  if (!scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 6;
  }

  request_scene_open(targetPath);
  if (!scene_document_prompt_open()) {
    editor_set_world(nullptr);
    return 7;
  }

  scene_document_prompt_choose_save();
  const bool ok = !scene_document_prompt_open() && !scene_document_is_dirty() &&
                  scene_document_has_path() &&
                  (std::strcmp(scene_document_path(), targetPath) == 0) &&
                  (world->find_entity_by_name("TargetEntity") !=
                   kInvalidEntity);
  editor_set_world(nullptr);
  if (!ok) {
    return 8;
  }

  // The resave to currentPath before switching must have actually landed
  // on disk (not just cleared the in-memory dirty flag).
  std::unique_ptr<World> verifyWorld(new (std::nothrow) World());
  if ((verifyWorld == nullptr) || !load_scene(*verifyWorld, currentPath) ||
      (verifyWorld->find_entity_by_name("CurrentEntity") == kInvalidEntity)) {
    return 9;
  }
  return 0;
}

/// EXPECTATION: choosing Save on an untitled, dirty document defers to a
/// Save As dialog; simulating that dialog's async result (without
/// invoking the real native picker) exercises the exact production
/// continuation path: the document is saved to disk, and then New still
/// resets the active document back to a fresh Untitled Scene — saving
/// answers "keep my work", not "make the saved file the new document".
int check_dirty_new_save_as_continuation_via_simulated_dialog() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char untitledSavePath[512] = {};
  if (!make_scratch_path("untitled_save_target.json", untitledSavePath,
                         sizeof(untitledSavePath))) {
    return 2;
  }
  static_cast<void>(std::remove(untitledSavePath));

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 3;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "UntitledEntity");
  if ((entity == kInvalidEntity) || !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 4;
  }
  if (scene_document_has_path()) {
    editor_set_world(nullptr);
    return 5;
  }

  request_scene_new();
  if (!scene_document_prompt_open()) {
    editor_set_world(nullptr);
    return 6;
  }

  // scene_document_prompt_choose_save() would call the real SDL Save As
  // dialog here (untitled document); simulate its async result instead,
  // exercising scene_document_poll_dialog_result()'s continuation branch
  // exactly as the real callback handoff would drive it.
  SceneDocumentState &doc = editor_session().document;
  doc.pendingAction = PendingSceneAction::New;
  doc.unsavedPromptOpen = true;
  doc.dialogContinuesPendingAction = true;
  doc.dialogPendingKind = SceneDialogKind::SaveAs;
  std::snprintf(doc.dialogResultPath, sizeof(doc.dialogResultPath), "%s",
               untitledSavePath);
  doc.dialogResultAccepted = true;
  doc.dialogResultPending.store(true, std::memory_order_release);

  scene_document_poll_dialog_result();

  const bool ok = !scene_document_prompt_open() && !scene_document_has_path() &&
                  !scene_document_is_dirty() &&
                  (world->alive_entity_count() == 0U) &&
                  (std::strcmp(scene_document_display_name(),
                              "Untitled Scene") == 0);
  editor_set_world(nullptr);
  if (!ok) {
    return 7;
  }

  std::unique_ptr<World> verifyWorld(new (std::nothrow) World());
  if ((verifyWorld == nullptr) || !load_scene(*verifyWorld, untitledSavePath) ||
      (verifyWorld->find_entity_by_name("UntitledEntity") ==
       kInvalidEntity)) {
    return 8;
  }
  return 0;
}

/// EXPECTATION: a save failure at prompt-resolution time keeps the prompt
/// armed (and the pending action intact) instead of silently dropping it
/// or proceeding as if the save had succeeded.
int check_save_failure_keeps_prompt_armed() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Unwritable");
  if ((entity == kInvalidEntity) || !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 2;
  }

  // Force a titled document whose path cannot be written (no such
  // directory) without exercising the real Save As dialog.
  SceneDocumentState &doc = editor_session().document;
  std::snprintf(doc.path, sizeof(doc.path),
               "/engine_scene_document_prompt_test_nonexistent_dir/x.json");
  doc.hasPath = true;

  request_scene_open("/also/does/not/matter.json"); // arms Save/Discard/Cancel
  if (!scene_document_prompt_open()) {
    editor_set_world(nullptr);
    return 3;
  }

  scene_document_prompt_choose_save();
  const bool ok = scene_document_prompt_open() &&
                  (scene_document_last_error()[0] != '\0') &&
                  scene_document_is_dirty();
  editor_set_world(nullptr);
  return ok ? 0 : 4;
}

/// EXPECTATION: the runtime quit-gate contract: request_scene_quit()
/// returns true immediately on a clean document; on a dirty document it
/// returns false and arms the same prompt, Cancel leaves the world/
/// document untouched, and Discard closes the prompt without touching
/// the world (only the deferred platform-quit call itself, which is
/// runtime glue outside this layer's contract).
int check_quit_gate_defers_while_dirty() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  editor_set_world(world.get());

  if (!request_scene_quit()) {
    editor_set_world(nullptr);
    return 2; // clean document must allow an immediate quit
  }

  const Entity entity = add_named_entity(*world, "QuitGuard");
  if ((entity == kInvalidEntity) || !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 3;
  }

  if (request_scene_quit()) {
    editor_set_world(nullptr);
    return 4; // dirty document must defer, not allow immediate quit
  }
  if (!scene_document_prompt_open()) {
    editor_set_world(nullptr);
    return 5;
  }

  scene_document_prompt_choose_cancel();
  if (scene_document_prompt_open() || (world->alive_entity_count() == 0U)) {
    editor_set_world(nullptr);
    return 6;
  }

  if (request_scene_quit()) {
    editor_set_world(nullptr);
    return 7;
  }
  scene_document_prompt_choose_discard();
  const bool ok = !scene_document_prompt_open() &&
                  (world->alive_entity_count() != 0U) &&
                  scene_document_is_dirty();
  editor_set_world(nullptr);
  return ok ? 0 : 8;
}

} // namespace

/// Runs this executable or test program.
int main() {
  struct NamedCheck {
    const char *name;
    int (*fn)();
  };
  const NamedCheck checks[] = {
      {"check_clean_document_actions_run_immediately",
       &check_clean_document_actions_run_immediately},
      {"check_dirty_new_arms_prompt_and_cancel_restores",
       &check_dirty_new_arms_prompt_and_cancel_restores},
      {"check_dirty_new_discard_performs_new",
       &check_dirty_new_discard_performs_new},
      {"check_dirty_open_save_continues_with_titled_document",
       &check_dirty_open_save_continues_with_titled_document},
      {"check_dirty_new_save_as_continuation_via_simulated_dialog",
       &check_dirty_new_save_as_continuation_via_simulated_dialog},
      {"check_save_failure_keeps_prompt_armed",
       &check_save_failure_keeps_prompt_armed},
      {"check_quit_gate_defers_while_dirty",
       &check_quit_gate_defers_while_dirty},
  };

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr,
                   "editor_scene_document_prompt_test: %s failed: %d\n",
                   check.name, result);
      return result;
    }
  }

  std::printf("editor_scene_document_prompt_test: all tests passed\n");
  return 0;
}
