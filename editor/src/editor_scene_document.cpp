// Implements the editor's scene-document identity, dirty-state tracking,
// and file-operation state machine (New/Open/Save/Save As, recent scenes,
// unsaved-change gating, async native file dialogs; ).

#include "editor_scene_document.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&      \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif


#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "editor_commands.h"
#include "editor_material_edit.h"
#include "editor_session.h"

namespace engine::editor {

namespace {

constexpr const char *kLogChannel = "editor.scene_document";
/// Clears identity/dirty/pending-prompt fields only; the recent-scenes
/// cache and any in-flight dialog state are session-lifetime and survive
/// New/Open (see the field comments in SceneDocumentState).
void reset_document_identity(SceneDocumentState &doc) noexcept {
  doc.path[0] = '\0';
  doc.hasPath = false;
  std::snprintf(doc.displayName, sizeof(doc.displayName), "Untitled Scene");
  doc.savedHistoryToken = 0U;
  doc.unrecordedEdit = false;
  doc.unsavedPromptOpen = false;
  doc.pendingAction = PendingSceneAction::None;
  doc.pendingOpenPath[0] = '\0';
  doc.lastSaveError[0] = '\0';
}

void set_display_name_from_path(SceneDocumentState &doc,
                                const char *path) noexcept {
  const std::filesystem::path parsed(path);
  const std::filesystem::path filename = parsed.filename();
  if (filename.empty()) {
    std::snprintf(doc.displayName, sizeof(doc.displayName), "Untitled Scene");
  } else {
    std::snprintf(doc.displayName, sizeof(doc.displayName), "%s",
                  filename.string().c_str());
  }
}

/// Resets the parts of the session other than document identity that a
/// scene switch (New or Open) must partition away: pending inspector
/// gestures, selection, undo history, and any stale Play snapshot. Play
/// state itself is untouched — callers only reach here while stopped.
void reset_session_for_scene_switch() noexcept {
  EditorSession &session = editor_session();
  inspector_abandon_pending_edit();
  gizmo_abandon_gesture();
  clear_entity_selection();
  session.commandHistory.clear();
  session.document.unrecordedEdit = false;
  ++session.documentGeneration;
  session.worldRestoreFailed = false;
  session.hasPlaySnapshot = false;
  session.playSnapshotSize = 0U;
  session.playSnapshotWorld = nullptr;
  session.playStopPending = false;
}

void arm_pending_action(PendingSceneAction action, const char *path) noexcept {
  SceneDocumentState &doc = editor_session().document;
  doc.pendingAction = action;
  doc.pendingOpenPath[0] = '\0';
  if (path != nullptr) {
    std::snprintf(doc.pendingOpenPath, sizeof(doc.pendingOpenPath), "%s",
                  path);
  }
  doc.unsavedPromptOpen = true;
}

/// Executes the armed PendingSceneAction and clears the prompt/pending
/// state; called once the unsaved-change prompt has been resolved in
/// favor of proceeding (Save succeeded, or the user chose Discard).
void continue_pending_action() noexcept {
  SceneDocumentState &doc = editor_session().document;
  const PendingSceneAction action = doc.pendingAction;
  doc.pendingAction = PendingSceneAction::None;
  doc.unsavedPromptOpen = false;

  switch (action) {
  case PendingSceneAction::New:
    static_cast<void>(perform_scene_new());
    break;
  case PendingSceneAction::OpenPath:
    static_cast<void>(perform_scene_open(doc.pendingOpenPath));
    break;
  case PendingSceneAction::Quit:
    core::request_platform_quit();
    break;
  case PendingSceneAction::None:
  default:
    break;
  }
  doc.pendingOpenPath[0] = '\0';
}

/// Asks the platform for a scene dialog and makes it the one the session
/// waits on. False when the platform refused; it logged why.
bool begin_scene_dialog(SceneDocumentState &doc, SceneDialogKind kind,
                        const char *defaultLocation) noexcept {
  static const core::FileDialogFilter kFilters[] = {{"Scene", "scene"}};
  const core::FileDialogTicket ticket = core::platform_request_file_dialog(
      (kind == SceneDialogKind::Open) ? core::FileDialogKind::Open
                                      : core::FileDialogKind::Save,
      kFilters, 1, defaultLocation);
  if (ticket == core::kNoFileDialog) {
    return false;
  }
  doc.activeDialog = ticket;
  doc.dialogPendingKind = kind;
  return true;
}

void begin_save_scene_as_dialog() noexcept {
  EditorSession &session = editor_session();
  SceneDocumentState &doc = session.document;
  if (doc.dialogPendingKind != SceneDialogKind::None) {
    return; // one native dialog at a time
  }
  const char *defaultLocation =
      doc.hasPath ? doc.path : editor_asset_root();
  if (!begin_scene_dialog(doc, SceneDialogKind::SaveAs, defaultLocation) &&
      doc.dialogContinuesPendingAction) {
    // No dialog will answer, so treat it as dismissed: the action waiting
    // on it is cancelled, exactly as if the user had closed the dialog.
    doc.dialogContinuesPendingAction = false;
    scene_document_prompt_choose_cancel();
  }
}

} // namespace

const char *scene_document_display_name() noexcept {
  return editor_session().document.displayName;
}

const char *scene_document_path() noexcept {
  const SceneDocumentState &doc = editor_session().document;
  return doc.hasPath ? doc.path : "";
}

bool scene_document_has_path() noexcept {
  return editor_session().document.hasPath;
}

bool scene_document_is_dirty() noexcept {
  const EditorSession &session = editor_session();
  // After a failed Stop restore the world is the preserved play world,
  // not the file on disk: replacing it unasked would discard the state
  // the recovery path exists to keep, so it counts as unsaved.
  return session.worldRestoreFailed || session.document.unrecordedEdit ||
         (session.commandHistory.current_token() !=
          session.document.savedHistoryToken);
}

const char *scene_document_last_error() noexcept {
  return editor_session().document.lastSaveError;
}

bool perform_scene_new() noexcept {
  EditorSession &session = editor_session();
  if (!world_can_load_scene()) {
    return false;
  }

  runtime::reset_world(*session.world);
  reset_session_for_scene_switch();
  reset_document_identity(session.document);
  return true;
}

bool perform_scene_open(const char *path) noexcept {
  EditorSession &session = editor_session();
  if ((path == nullptr) || (path[0] == '\0') || !world_can_load_scene()) {
    return false;
  }

  if (!runtime::load_scene(*session.world, path)) {
    // load_scene is transactional: the live world, document identity, and
    // undo history are all still exactly as they were before this call.
    recent_list_remove(&editor_session().document.recentScenes, path);
    return false;
  }

  reset_session_for_scene_switch();
  std::snprintf(session.document.path, sizeof(session.document.path), "%s",
               path);
  session.document.hasPath = true;
  set_display_name_from_path(session.document, path);
  session.document.savedHistoryToken = session.commandHistory.current_token();
  session.document.unrecordedEdit = false;
  session.document.unsavedPromptOpen = false;
  session.document.pendingAction = PendingSceneAction::None;
  session.document.pendingOpenPath[0] = '\0';
  session.document.lastSaveError[0] = '\0';
  recent_scenes_add(path);
  return true;
}

/// Composes the failed-save status message: state the scene format cannot
/// represent gets its precise counts; anything else was a write
/// failure on the destination path.
void set_save_failure_message(EditorSession &session,
                              const char *path) noexcept {
  const runtime::SceneSaveBlockers blockers =
      runtime::collect_scene_save_blockers(*session.world);
  if ((blockers.customHullPayloads > 0U) ||
      (blockers.heightfieldPayloads > 0U) || (blockers.activeJoints > 0U)) {
    std::snprintf(session.document.lastSaveError,
                  sizeof(session.document.lastSaveError),
                  "cannot save: %zu custom hull payload(s), %zu heightfield "
                  "payload(s), %zu active joint(s) are runtime-only state "
                  "the scene format cannot keep",
                  blockers.customHullPayloads, blockers.heightfieldPayloads,
                  blockers.activeJoints);
    return;
  }
  std::snprintf(session.document.lastSaveError,
                sizeof(session.document.lastSaveError), "failed to write %s",
                path);
}

/// Every failed save is said twice: the status stays in lastSaveError
/// for the menu bar and the unsaved-changes prompt, and one Error line
/// goes to the log, so a Save As that wrote nothing is never read as
/// "saved" from a title that merely kept its dirty marker.
bool report_save_failure(EditorSession &session) noexcept {
  char message[sizeof(session.document.lastSaveError) + 32U] = {};
  std::snprintf(message, sizeof(message), "scene save failed: %s",
                session.document.lastSaveError);
  core::log_message(core::LogLevel::Error, "editor", message);
  return false;
}

bool perform_scene_save() noexcept {
  EditorSession &session = editor_session();
  // Every refusal states its reason: the quit prompt's Save button reads
  // lastSaveError, and a silent false looked like a button that did
  // nothing.
  if (!session.document.hasPath) {
    std::snprintf(session.document.lastSaveError,
                  sizeof(session.document.lastSaveError),
                  "the scene has no path yet; use Save As");
    return report_save_failure(session);
  }
  if (!world_is_editable()) {
    std::snprintf(session.document.lastSaveError,
                  sizeof(session.document.lastSaveError),
                  session.worldRestoreFailed
                      ? "the Stop restore failed; use Save As to export the "
                        "preserved world, or New/Open to replace it"
                      : "the scene cannot be saved while playing");
    return report_save_failure(session);
  }
  if (!runtime::save_scene(*session.world, session.document.path)) {
    set_save_failure_message(session, session.document.path);
    return report_save_failure(session);
  }
  session.document.savedHistoryToken = session.commandHistory.current_token();
  session.document.unrecordedEdit = false;
  session.document.lastSaveError[0] = '\0';
  return true;
}

bool scene_path_passes_jail_under(const char *path,
                                  const char *root) noexcept {
  if ((path == nullptr) || (path[0] == '\0') || (root == nullptr) ||
      (root[0] == '\0')) {
    return false;
  }

  namespace fs = std::filesystem;
  std::error_code rootEc{};
  const fs::path canonicalRoot = fs::weakly_canonical(fs::path(root), rootEc);
  if (rootEc) {
    return false;
  }

  const fs::path candidate(path);
  std::error_code parentEc{};
  const fs::path canonicalParent =
      fs::weakly_canonical(candidate.parent_path(), parentEc);
  if (parentEc) {
    return false;
  }

  auto rootIt = canonicalRoot.begin();
  auto parentIt = canonicalParent.begin();
  for (; rootIt != canonicalRoot.end(); ++rootIt, ++parentIt) {
    if ((parentIt == canonicalParent.end()) || (*parentIt != *rootIt)) {
      return false;
    }
  }
  return true;
}

bool scene_path_passes_jail(const char *path) noexcept {
  return scene_path_passes_jail_under(path, editor_asset_root());
}

bool perform_scene_save_as(const char *path) noexcept {
  EditorSession &session = editor_session();
  if ((path == nullptr) || (path[0] == '\0')) {
    std::snprintf(session.document.lastSaveError,
                  sizeof(session.document.lastSaveError),
                  "Save As needs a destination path");
    return report_save_failure(session);
  }
  if (!world_can_load_scene()) {
    std::snprintf(session.document.lastSaveError,
                  sizeof(session.document.lastSaveError),
                  "the scene cannot be exported while playing");
    return report_save_failure(session);
  }
  if (session.worldRestoreFailed) {
    // The export is the recovery path; the author is told what the
    // file will hold, since it is the preserved play world, not the scene
    // as it was before Play.
    core::log_message(core::LogLevel::Warning, "editor",
                      "Save As after a failed Stop restore exports the "
                      "preserved play-mode world, not the pre-Play scene");
  }
  if (!scene_path_passes_jail(path)) {
    std::snprintf(session.document.lastSaveError,
                  sizeof(session.document.lastSaveError),
                  "destination %s is outside the project asset root", path);
    return report_save_failure(session);
  }
  // Whether this destination already existed decides what rolling back
  // means below: a file the author already had must survive a failure
  // here, and one this save created must not outlive it.
  std::error_code existsEc{};
  const bool destinationExisted = std::filesystem::exists(path, existsEc);

  if (!runtime::save_scene(*session.world, path)) {
    set_save_failure_message(session, path);
    return report_save_failure(session);
  }

  // A scene is referenceable, so it needs the identity a reference names,
  // and it needs it from the same transaction that wrote it rather than
  // from a tool the author is expected to run afterwards. The diagnostic
  // for each failing case is logged by the bridge.
  const runtime::EditorIdentityResult identity =
      runtime::editor_establish_asset_identity(path);
  if ((identity != runtime::EditorIdentityResult::Created) &&
      (identity != runtime::EditorIdentityResult::AlreadyIdentified)) {
    if (!destinationExisted) {
      // Nothing referenced this path a moment ago, so removing it leaves
      // the project exactly as it was instead of leaving a scene behind
      // that nothing can name.
      std::error_code removeEc{};
      static_cast<void>(std::filesystem::remove(path, removeEc));
    }
    std::snprintf(session.document.lastSaveError,
                  sizeof(session.document.lastSaveError),
                  "%s was written but could not be given an identity, so it "
                  "could not be referenced",
                  path);
    return report_save_failure(session);
  }

  if (session.worldRestoreFailed) {
    // The export ends the recovery: the world now matches the file just
    // written, so it becomes the document exactly as if it had been
    // opened, and the undo history, which described the pre-Play world,
    // goes with the latch.
    reset_session_for_scene_switch();
  }
  std::snprintf(session.document.path, sizeof(session.document.path), "%s",
               path);
  session.document.hasPath = true;
  set_display_name_from_path(session.document, path);
  session.document.savedHistoryToken = session.commandHistory.current_token();
  session.document.unrecordedEdit = false;
  session.document.lastSaveError[0] = '\0';
  recent_scenes_add(path);
  return true;
}

// The request entry points gate on world_can_load_scene, not
// world_is_editable: after a failed Stop restore, New, Open and Save As
// are the recovery path, and only Play and in-place Save stay refused.
void request_scene_new() noexcept {
  if (!world_can_load_scene()) {
    return;
  }
  if (!scene_document_is_dirty()) {
    static_cast<void>(perform_scene_new());
    return;
  }
  arm_pending_action(PendingSceneAction::New, nullptr);
}

void request_scene_open(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return;
  }
  if (!world_can_load_scene()) {
    // Reachable when an Open dialog answers after Play began.
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "a scene cannot be opened while playing; stop first");
    return;
  }
  if (!scene_document_is_dirty()) {
    static_cast<void>(perform_scene_open(path));
    return;
  }
  arm_pending_action(PendingSceneAction::OpenPath, path);
}

bool request_scene_quit() noexcept {
  // Quit ends every document at once, so the gate covers the material
  // document too; New/Open replace only the scene and leave an open
  // material (and its dirty state) untouched, so they gate on the scene
  // alone.
  if (!scene_document_is_dirty() && !material_editor_is_dirty()) {
    return true;
  }
  arm_pending_action(PendingSceneAction::Quit, nullptr);
  return false;
}

bool scene_document_prompt_open() noexcept {
  return editor_session().document.unsavedPromptOpen;
}

bool scene_document_prompt_covers_material() noexcept {
  const SceneDocumentState &doc = editor_session().document;
  return doc.unsavedPromptOpen &&
         (doc.pendingAction == PendingSceneAction::Quit) &&
         material_editor_is_dirty();
}

void scene_document_prompt_choose_save() noexcept {
  SceneDocumentState &doc = editor_session().document;
  if (scene_document_prompt_covers_material()) {
    if (!save_material_editor()) {
      // The prompt stays armed with the material's error, exactly as a
      // failed scene save keeps it: nothing is quit while any covered
      // document is still unsaved.
      std::snprintf(doc.lastSaveError, sizeof(doc.lastSaveError), "%s",
                    material_editor_state().lastSaveError);
      return;
    }
    doc.lastSaveError[0] = '\0';
  }
  if (!scene_document_is_dirty()) {
    // Only the material was unsaved; it is persisted now.
    continue_pending_action();
    return;
  }
  // After a failed Stop restore an in-place save is refused (it would
  // overwrite the scene with the play world), so the prompt's Save is an
  // export through Save As instead.
  if (doc.hasPath && !editor_session().worldRestoreFailed) {
    if (perform_scene_save()) {
      continue_pending_action();
    }
    // Save failed: doc.lastSaveError is set for the UI; the prompt stays
    // armed so the user can retry (fix disk space, permissions, ...) or
    // fall back to Cancel/Discard.
    return;
  }
  doc.dialogContinuesPendingAction = true;
  begin_save_scene_as_dialog();
}

void scene_document_prompt_choose_discard() noexcept {
  continue_pending_action();
}

void scene_document_prompt_choose_cancel() noexcept {
  SceneDocumentState &doc = editor_session().document;
  doc.pendingAction = PendingSceneAction::None;
  doc.unsavedPromptOpen = false;
  doc.pendingOpenPath[0] = '\0';
}

void request_open_scene_dialog() noexcept {
  SceneDocumentState &doc = editor_session().document;
  if (!world_can_load_scene() ||
      (doc.dialogPendingKind != SceneDialogKind::None)) {
    return;
  }
  static_cast<void>(
      begin_scene_dialog(doc, SceneDialogKind::Open, editor_asset_root()));
}

void request_save_scene() noexcept {
  if (!world_can_load_scene()) {
    return;
  }
  // With the restore latch set, perform_scene_save refuses and says why
  // in lastSaveError; an untitled document goes straight to Save As.
  if (editor_session().document.hasPath) {
    static_cast<void>(perform_scene_save());
    return;
  }
  editor_session().document.dialogContinuesPendingAction = false;
  begin_save_scene_as_dialog();
}

void request_save_scene_as() noexcept {
  if (!world_can_load_scene()) {
    return;
  }
  editor_session().document.dialogContinuesPendingAction = false;
  begin_save_scene_as_dialog();
}

void scene_document_poll_dialog_result() noexcept {
  SceneDocumentState &doc = editor_session().document;
  if (doc.activeDialog == core::kNoFileDialog) {
    return;
  }
  // Static: a result carries a path buffer too large for the frame's stack.
  static core::FileDialogResult result{};
  if (core::platform_take_file_dialog_result(doc.activeDialog, &result) ==
      core::FileDialogPoll::Pending) {
    return;
  }
  // Ready, or Unknown if the platform lost the ticket; either way the
  // session stops waiting, and anything but a chosen path is a cancel.
  const SceneDialogKind kind = doc.dialogPendingKind;
  doc.dialogPendingKind = SceneDialogKind::None;
  doc.activeDialog = core::kNoFileDialog;
  const bool continues = doc.dialogContinuesPendingAction;
  doc.dialogContinuesPendingAction = false;

  // The document's path is what Save As writes to and Open reads from,
  // so a path that does not fit is refused, not cut: a cut path names a
  // different file.
  bool accepted = (result.ticket != core::kNoFileDialog) &&
                  (result.outcome == core::FileDialogOutcome::Chosen);
  char path[kMaxDocumentPathLength] = {};
  if (accepted) {
    const std::size_t length = std::strlen(result.path);
    if (length < sizeof(path)) {
      std::memcpy(path, result.path, length + 1U);
    } else {
      accepted = false;
      core::log_message(core::LogLevel::Error, kLogChannel,
                        "the chosen path is longer than the editor can hold; "
                        "nothing was opened or saved");
    }
  }
  result = core::FileDialogResult{};

  if (!accepted) {
    if (continues) {
      // The user canceled the Save As dialog that was resolving the
      // unsaved-change prompt: cancel the whole pending action too, the
      // same as a native app's Cancel button.
      scene_document_prompt_choose_cancel();
    }
    return;
  }

  if (kind == SceneDialogKind::Open) {
    request_scene_open(path);
  } else if (kind == SceneDialogKind::SaveAs) {
    if (perform_scene_save_as(path) && continues) {
      continue_pending_action();
    }
  }
}

void recent_scenes_load_once() noexcept {
  recent_list_load_once(&editor_session().document.recentScenes);
}

void recent_scenes_add(const char *path) noexcept {
  recent_list_add(&editor_session().document.recentScenes, path);
}

std::size_t recent_scene_count() noexcept {
  return recent_list_count(&editor_session().document.recentScenes);
}

const char *recent_scene_at(std::size_t index) noexcept {
  return recent_list_at(&editor_session().document.recentScenes, index);
}

void scene_document_update_window_title() noexcept {
  EditorSession &session = editor_session();
  if (!session.initialized) {
    return;
  }
  char title[640] = {};
  std::snprintf(title, sizeof(title), "Engine Editor - %s%s",
               scene_document_display_name(),
               scene_document_is_dirty() ? " *" : "");
  if (std::strcmp(title, session.lastAppliedWindowTitle) == 0) {
    return;
  }
  // Recorded only when applied, so a refused title is retried next frame
  // rather than believed.
  if (!core::platform_set_window_title(title)) {
    return;
  }
  std::snprintf(session.lastAppliedWindowTitle,
               sizeof(session.lastAppliedWindowTitle), "%s", title);
}

void scene_document_reset_for_world_switch() noexcept {
  reset_document_identity(editor_session().document);
}

void scene_document_retire_dialogs() noexcept {
  SceneDocumentState &doc = editor_session().document;
  core::platform_abandon_file_dialog(doc.activeDialog);
  doc.activeDialog = core::kNoFileDialog;
  doc.dialogPendingKind = SceneDialogKind::None;
  doc.dialogContinuesPendingAction = false;
}

core::FileDialogTicket
scene_dialog_arm_for_tests(SceneDialogKind kind,
                           bool continuesPendingAction) noexcept {
  SceneDocumentState &doc = editor_session().document;
  if ((kind == SceneDialogKind::None) ||
      (doc.dialogPendingKind != SceneDialogKind::None)) {
    return core::kNoFileDialog;
  }
  core::platform_set_scripted_file_dialogs(true);
  if (kind == SceneDialogKind::Open) {
    request_open_scene_dialog();
  } else {
    doc.dialogContinuesPendingAction = continuesPendingAction;
    begin_save_scene_as_dialog();
  }
  core::platform_set_scripted_file_dialogs(false);
  return doc.activeDialog;
}

void scene_dialog_deliver_for_tests(core::FileDialogTicket ticket,
                                    const char *path) noexcept {
  static_cast<void>(core::platform_answer_scripted_file_dialog(ticket, path));
}

void recent_scenes_set_directory_override_for_tests(
    const char *directory) noexcept {
  recent_lists_set_directory_override_for_tests(directory);
  // A new directory invalidates the cache, so the next access reads the
  // (possibly empty) overridden location.
  recent_list_forget(&editor_session().document.recentScenes);
}

} // namespace engine::editor
