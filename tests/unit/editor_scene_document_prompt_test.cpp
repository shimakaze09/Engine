// Verifies the unsaved-change confirm-prompt state machine (issue #158):
// New/Open/quit run immediately on a clean document, dirty documents arm
// the Save/Discard/Cancel prompt instead, Cancel leaves everything
// untouched, Discard proceeds without saving, and Save either saves in
// place (titled document) or resolves through the async Save As dialog
// handoff (untitled document) before continuing the deferred action. A
// failed Save keeps the prompt armed instead of silently losing the
// pending action. It also pins the dialog-session boundary (audit #390):
// a native dialog result that arrives after its session was retired is
// discarded, a fresh dialog afterwards completes normally, and request
// records are never reused while a callback may still write them. ImGui
// draw code is out of scope here (exempt per CLAUDE.md); every check
// below drives the production functions the panel calls, not a copy.

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
  // dialog here (untitled document); arm the same request without the
  // native call and deliver its result through the production callback,
  // exercising scene_document_poll_dialog_result()'s continuation branch
  // exactly as the real callback handoff would drive it.
  SceneDocumentState &doc = editor_session().document;
  doc.pendingAction = PendingSceneAction::New;
  doc.unsavedPromptOpen = true;
  void *request = scene_dialog_arm_for_tests(SceneDialogKind::SaveAs, true);
  if (request == nullptr) {
    editor_set_world(nullptr);
    return 9;
  }
  scene_dialog_deliver_for_tests(request, untitledSavePath);

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

/// Writes a scene holding one named entity to `path`; false on failure.
bool write_scene_with_entity(const char *path, const char *name) noexcept {
  static_cast<void>(std::remove(path));
  std::unique_ptr<World> writer(new (std::nothrow) World());
  return (writer != nullptr) &&
         (add_named_entity(*writer, name) != kInvalidEntity) &&
         save_scene(*writer, path);
}

/// EXPECTATION (audit #390): a native dialog result that arrives after
/// the session that opened it was shut down is discarded — it neither
/// opens the chosen scene into the later session's World nor saves the
/// later session's World to the chosen path — and the later session
/// waits on no dialog afterwards. Red on base: the callback published
/// into process-global session storage and the next poll acted on it.
int check_retired_dialog_result_is_discarded() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char staleOpen[512] = {};
  char staleSave[512] = {};
  if (!make_scratch_path("stale_open.json", staleOpen, sizeof(staleOpen)) ||
      !make_scratch_path("stale_save.json", staleSave, sizeof(staleSave)) ||
      !write_scene_with_entity(staleOpen, "FromRetiredSession")) {
    return 2;
  }
  static_cast<void>(std::remove(staleSave));

  // First session: an Open dialog is outstanding when the editor shuts
  // down (shutdown_editor runs scene_document_retire_dialogs).
  std::unique_ptr<World> first(new (std::nothrow) World());
  if (first == nullptr) {
    return 3;
  }
  editor_set_world(first.get());
  void *staleOpenRequest =
      scene_dialog_arm_for_tests(SceneDialogKind::Open, false);
  if (staleOpenRequest == nullptr) {
    editor_set_world(nullptr);
    return 4;
  }
  scene_document_retire_dialogs();
  editor_set_world(nullptr);

  // Later session in the same process, then the delayed callback.
  std::unique_ptr<World> later(new (std::nothrow) World());
  if (later == nullptr) {
    return 5;
  }
  editor_set_world(later.get());
  scene_dialog_deliver_for_tests(staleOpenRequest, staleOpen);
  scene_document_poll_dialog_result();
  if ((later->find_entity_by_name("FromRetiredSession") != kInvalidEntity) ||
      scene_document_has_path() ||
      (editor_session().document.dialogPendingKind !=
       SceneDialogKind::None)) {
    editor_set_world(nullptr);
    return 6;
  }

  // Same for Save As: the retired dialog's path must not receive the
  // later session's World.
  if (add_named_entity(*later, "LaterEntity") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 7;
  }
  void *staleSaveRequest =
      scene_dialog_arm_for_tests(SceneDialogKind::SaveAs, false);
  if (staleSaveRequest == nullptr) {
    editor_set_world(nullptr);
    return 8;
  }
  scene_document_retire_dialogs();
  editor_set_world(nullptr);

  std::unique_ptr<World> third(new (std::nothrow) World());
  if ((third == nullptr) ||
      (add_named_entity(*third, "ThirdEntity") == kInvalidEntity)) {
    return 9;
  }
  editor_set_world(third.get());
  scene_dialog_deliver_for_tests(staleSaveRequest, staleSave);
  scene_document_poll_dialog_result();
  std::error_code ec{};
  const bool ok = !std::filesystem::exists(staleSave, ec) &&
                  !scene_document_has_path() &&
                  (editor_session().document.dialogPendingKind ==
                   SceneDialogKind::None);
  editor_set_world(nullptr);
  return ok ? 0 : 10;
}

/// EXPECTATION (audit #390): after a retirement a fresh dialog still
/// completes normally, and a retired dialog's late result cannot hijack
/// the fresh request even when it arrives while that request is
/// outstanding — accepted and cancelled results alike.
int check_fresh_dialog_after_retire_completes() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char stalePath[512] = {};
  char freshPath[512] = {};
  if (!make_scratch_path("stale_late.json", stalePath, sizeof(stalePath)) ||
      !make_scratch_path("fresh_open.json", freshPath, sizeof(freshPath)) ||
      !write_scene_with_entity(stalePath, "StaleLate") ||
      !write_scene_with_entity(freshPath, "FreshOpen")) {
    return 2;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 3;
  }
  editor_set_world(world.get());
  void *stale = scene_dialog_arm_for_tests(SceneDialogKind::Open, false);
  if (stale == nullptr) {
    editor_set_world(nullptr);
    return 4;
  }
  scene_document_retire_dialogs();

  void *fresh = scene_dialog_arm_for_tests(SceneDialogKind::Open, false);
  if ((fresh == nullptr) || (fresh == stale)) {
    editor_set_world(nullptr);
    return 5;
  }
  // The retired dialog reports late, while the fresh one is outstanding.
  scene_dialog_deliver_for_tests(stale, stalePath);
  scene_document_poll_dialog_result();
  if ((world->find_entity_by_name("StaleLate") != kInvalidEntity) ||
      (editor_session().document.dialogPendingKind != SceneDialogKind::Open)) {
    editor_set_world(nullptr);
    return 6;
  }
  scene_dialog_deliver_for_tests(fresh, freshPath);
  scene_document_poll_dialog_result();
  if ((world->find_entity_by_name("FreshOpen") == kInvalidEntity) ||
      (std::strcmp(scene_document_path(), freshPath) != 0)) {
    editor_set_world(nullptr);
    return 7;
  }

  // Cancelled results: a retired cancel must not cancel the later
  // session's pending action, while the current session's cancel does.
  request_scene_new();
  void *staleCancel = scene_dialog_arm_for_tests(SceneDialogKind::SaveAs, true);
  if (staleCancel == nullptr) {
    editor_set_world(nullptr);
    return 8;
  }
  scene_document_retire_dialogs();
  editor_set_world(nullptr);

  std::unique_ptr<World> later(new (std::nothrow) World());
  if (later == nullptr) {
    return 9;
  }
  editor_set_world(later.get());
  const Entity entity = add_named_entity(*later, "Dirty");
  if ((entity == kInvalidEntity) || !push_transform_edit(*later, entity)) {
    editor_set_world(nullptr);
    return 10;
  }
  request_scene_new();
  if (!scene_document_prompt_open()) {
    editor_set_world(nullptr);
    return 11;
  }
  scene_dialog_deliver_for_tests(staleCancel, nullptr);
  scene_document_poll_dialog_result();
  if (!scene_document_prompt_open()) {
    editor_set_world(nullptr);
    return 12;
  }
  void *currentCancel =
      scene_dialog_arm_for_tests(SceneDialogKind::SaveAs, true);
  if (currentCancel == nullptr) {
    editor_set_world(nullptr);
    return 13;
  }
  scene_dialog_deliver_for_tests(currentCancel, nullptr);
  scene_document_poll_dialog_result();
  const bool ok = !scene_document_prompt_open() &&
                  (later->alive_entity_count() != 0U);
  editor_set_world(nullptr);
  return ok ? 0 : 14;
}

/// EXPECTATION (audit #390): request records are never aliased while a
/// callback may still write them. With every slot held by a retired
/// dialog that has not reported, a new dialog is refused; once one of
/// those dialogs reports, its slot is reclaimed and reused, and repeated
/// retirements keep the pool usable.
int check_request_pool_reclaims_delivered_retired_records() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  editor_set_world(world.get());

  void *held[kMaxSceneDialogRequests] = {};
  for (std::size_t i = 0U; i < kMaxSceneDialogRequests; ++i) {
    held[i] = scene_dialog_arm_for_tests(SceneDialogKind::Open, false);
    if (held[i] == nullptr) {
      editor_set_world(nullptr);
      return 2;
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if (held[j] == held[i]) {
        editor_set_world(nullptr);
        return 3;
      }
    }
    scene_document_retire_dialogs();
  }

  if (scene_dialog_arm_for_tests(SceneDialogKind::Open, false) != nullptr) {
    editor_set_world(nullptr);
    return 4; // every slot is still owned by an unreported dialog
  }

  scene_dialog_deliver_for_tests(held[1], nullptr);
  void *reclaimed = scene_dialog_arm_for_tests(SceneDialogKind::Open, false);
  if (reclaimed != held[1]) {
    editor_set_world(nullptr);
    return 5;
  }
  scene_document_retire_dialogs();
  scene_document_retire_dialogs();
  if (scene_dialog_arm_for_tests(SceneDialogKind::Open, false) != nullptr) {
    editor_set_world(nullptr);
    return 6;
  }
  for (std::size_t i = 0U; i < kMaxSceneDialogRequests; ++i) {
    scene_dialog_deliver_for_tests(held[i], nullptr);
  }
  void *fresh = scene_dialog_arm_for_tests(SceneDialogKind::Open, false);
  if (fresh == nullptr) {
    editor_set_world(nullptr);
    return 7;
  }
  scene_dialog_deliver_for_tests(fresh, nullptr);
  scene_document_poll_dialog_result();
  const bool ok =
      editor_session().document.dialogPendingKind == SceneDialogKind::None;
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
      {"check_retired_dialog_result_is_discarded",
       &check_retired_dialog_result_is_discarded},
      {"check_fresh_dialog_after_retire_completes",
       &check_fresh_dialog_after_retire_completes},
      {"check_request_pool_reclaims_delivered_retired_records",
       &check_request_pool_reclaims_delivered_retired_records},
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
