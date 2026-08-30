// Implements the editor's scene-document identity, dirty-state tracking,
// and file-operation state machine (New/Open/Save/Save As, recent scenes,
// unsaved-change gating, async native file dialogs; issue #158).

#include "editor_scene_document.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&      \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "editor_commands.h"
#include "editor_session.h"

namespace engine::editor {

namespace {

constexpr const char *kLogChannel = "editor.scene_document";
constexpr const char *kRecentScenesFileName = "editor_recent_scenes.json";

/// Test-only override directory for recent-scenes persistence; empty
/// means "use the real platform save directory". Single-threaded like
/// the rest of editor session state.
char g_recentScenesDirectoryOverride[900] = {};

/// Clears identity/dirty/pending-prompt fields only; the recent-scenes
/// cache and any in-flight dialog state are session-lifetime and survive
/// New/Open (see the field comments in SceneDocumentState).
void reset_document_identity(SceneDocumentState &doc) noexcept {
  doc.path[0] = '\0';
  doc.hasPath = false;
  std::snprintf(doc.displayName, sizeof(doc.displayName), "Untitled Scene");
  doc.savedHistoryToken = 0U;
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
  clear_entity_selection();
  session.commandHistory.clear();
  session.worldRestoreFailed = false;
  session.hasPlaySnapshot = false;
  session.playSnapshotSize = 0U;
  session.playSnapshotWorld = nullptr;
}

/// Resolves the recent-scenes persistence directory: the test override
/// when set, otherwise the real per-user platform save directory.
bool resolve_recent_scenes_directory(char *out, std::size_t capacity) noexcept {
  if (g_recentScenesDirectoryOverride[0] != '\0') {
    const int written =
        std::snprintf(out, capacity, "%s", g_recentScenesDirectoryOverride);
    return (written > 0) && (static_cast<std::size_t>(written) < capacity);
  }
  return core::platform_get_save_dir(out, capacity);
}

bool build_recent_scenes_path(char *out, std::size_t capacity) noexcept {
  char directory[900] = {};
  if (!resolve_recent_scenes_directory(directory, sizeof(directory))) {
    return false;
  }
  const int written =
      std::snprintf(out, capacity, "%s/%s", directory, kRecentScenesFileName);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

void recent_scenes_persist() noexcept {
  const SceneDocumentState &doc = editor_session().document;

  char directory[900] = {};
  if (resolve_recent_scenes_directory(directory, sizeof(directory)) &&
      !core::create_directories_durably(directory)) {
    return;
  }

  char path[1024] = {};
  if (!build_recent_scenes_path(path, sizeof(path))) {
    return;
  }

  core::JsonWriter writer{};
  writer.begin_object();
  writer.begin_array("scenes");
  for (std::size_t i = 0U; i < doc.recentSceneCount; ++i) {
    writer.write_string_value(doc.recentScenes[i]);
  }
  writer.end_array();
  writer.end_object();
  if (writer.failed()) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "failed to serialize recent scenes list");
    return;
  }

  if (!core::atomic_write_file(path, writer.result(), writer.result_size())) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "failed to write recent scenes list");
  }
}

/// Drops `path` from the recent list (no-op when absent) and persists.
void recent_scenes_remove(const char *path) noexcept {
  SceneDocumentState &doc = editor_session().document;
  std::size_t writeIndex = 0U;
  for (std::size_t i = 0U; i < doc.recentSceneCount; ++i) {
    if (std::strcmp(doc.recentScenes[i], path) == 0) {
      continue;
    }
    if (writeIndex != i) {
      // Rows never overlap (writeIndex < i on every reshuffle here), but
      // both are sub-objects of the same recentScenes array, so an
      // snprintf(dst, ..., "%s", src) pair the compiler cannot prove
      // disjoint trips -Wrestrict; std::memmove sidesteps that.
      std::memmove(doc.recentScenes[writeIndex], doc.recentScenes[i],
                  kMaxDocumentPathLength);
    }
    ++writeIndex;
  }
  if (writeIndex != doc.recentSceneCount) {
    doc.recentSceneCount = writeIndex;
    recent_scenes_persist();
  }
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

/// SDL_ShowOpenFileDialog/SDL_ShowSaveFileDialog callback: only publishes
/// the result through the atomic handoff (see the SceneDocumentState
/// comment); all filesystem work happens later on the main thread.
void scene_dialog_callback(void *userdata, const char *const *filelist,
                           int filter) noexcept {
  static_cast<void>(filter);
  auto *session = static_cast<EditorSession *>(userdata);
  if (session == nullptr) {
    return;
  }
  SceneDocumentState &doc = session->document;
  if ((filelist == nullptr) || (filelist[0] == nullptr)) {
    doc.dialogResultAccepted = false;
  } else {
    std::snprintf(doc.dialogResultPath, sizeof(doc.dialogResultPath), "%s",
                  filelist[0]);
    doc.dialogResultAccepted = true;
  }
  doc.dialogResultPending.store(true, std::memory_order_release);
}

void begin_save_scene_as_dialog() noexcept {
  EditorSession &session = editor_session();
  SceneDocumentState &doc = session.document;
  if (doc.dialogPendingKind != SceneDialogKind::None) {
    return; // one native dialog at a time
  }
  doc.dialogPendingKind = SceneDialogKind::SaveAs;
  doc.dialogResultPending.store(false, std::memory_order_relaxed);

  static const SDL_DialogFileFilter kFilters[] = {{"Scene", "json"}};
  const char *defaultLocation =
      doc.hasPath ? doc.path : editor_asset_root();
  SDL_ShowSaveFileDialog(&scene_dialog_callback, &session, session.sdlWindow,
                         kFilters, 1, defaultLocation);
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
  return session.commandHistory.current_token() !=
         session.document.savedHistoryToken;
}

const char *scene_document_last_error() noexcept {
  return editor_session().document.lastSaveError;
}

bool perform_scene_new() noexcept {
  EditorSession &session = editor_session();
  if (!world_is_editable()) {
    return false;
  }

  runtime::reset_world(*session.world);
  reset_session_for_scene_switch();
  reset_document_identity(session.document);
  return true;
}

bool perform_scene_open(const char *path) noexcept {
  EditorSession &session = editor_session();
  if ((path == nullptr) || (path[0] == '\0') || !world_is_editable()) {
    return false;
  }

  if (!runtime::load_scene(*session.world, path)) {
    // load_scene is transactional: the live world, document identity, and
    // undo history are all still exactly as they were before this call.
    recent_scenes_remove(path);
    return false;
  }

  reset_session_for_scene_switch();
  std::snprintf(session.document.path, sizeof(session.document.path), "%s",
               path);
  session.document.hasPath = true;
  set_display_name_from_path(session.document, path);
  session.document.savedHistoryToken = session.commandHistory.current_token();
  session.document.unsavedPromptOpen = false;
  session.document.pendingAction = PendingSceneAction::None;
  session.document.pendingOpenPath[0] = '\0';
  session.document.lastSaveError[0] = '\0';
  recent_scenes_add(path);
  return true;
}

/// Composes the failed-save status message: state the scene format cannot
/// represent gets its precise counts (#208); anything else was a write
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

bool perform_scene_save() noexcept {
  EditorSession &session = editor_session();
  if (!session.document.hasPath || !world_is_editable()) {
    return false;
  }
  if (!runtime::save_scene(*session.world, session.document.path)) {
    set_save_failure_message(session, session.document.path);
    return false;
  }
  session.document.savedHistoryToken = session.commandHistory.current_token();
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
  if ((path == nullptr) || (path[0] == '\0') || !world_is_editable()) {
    return false;
  }
  if (!scene_path_passes_jail(path)) {
    std::snprintf(session.document.lastSaveError,
                  sizeof(session.document.lastSaveError),
                  "destination is outside the project asset root");
    return false;
  }
  if (!runtime::save_scene(*session.world, path)) {
    set_save_failure_message(session, path);
    return false;
  }

  std::snprintf(session.document.path, sizeof(session.document.path), "%s",
               path);
  session.document.hasPath = true;
  set_display_name_from_path(session.document, path);
  session.document.savedHistoryToken = session.commandHistory.current_token();
  session.document.lastSaveError[0] = '\0';
  recent_scenes_add(path);
  return true;
}

void request_scene_new() noexcept {
  if (!world_is_editable()) {
    return;
  }
  if (!scene_document_is_dirty()) {
    static_cast<void>(perform_scene_new());
    return;
  }
  arm_pending_action(PendingSceneAction::New, nullptr);
}

void request_scene_open(const char *path) noexcept {
  if (!world_is_editable() || (path == nullptr) || (path[0] == '\0')) {
    return;
  }
  if (!scene_document_is_dirty()) {
    static_cast<void>(perform_scene_open(path));
    return;
  }
  arm_pending_action(PendingSceneAction::OpenPath, path);
}

bool request_scene_quit() noexcept {
  if (!scene_document_is_dirty()) {
    return true;
  }
  arm_pending_action(PendingSceneAction::Quit, nullptr);
  return false;
}

bool scene_document_prompt_open() noexcept {
  return editor_session().document.unsavedPromptOpen;
}

void scene_document_prompt_choose_save() noexcept {
  SceneDocumentState &doc = editor_session().document;
  if (doc.hasPath) {
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
  EditorSession &session = editor_session();
  SceneDocumentState &doc = session.document;
  if (doc.dialogPendingKind != SceneDialogKind::None) {
    return;
  }
  doc.dialogPendingKind = SceneDialogKind::Open;
  doc.dialogResultPending.store(false, std::memory_order_relaxed);

  static const SDL_DialogFileFilter kFilters[] = {{"Scene", "json"}};
  SDL_ShowOpenFileDialog(&scene_dialog_callback, &session, session.sdlWindow,
                        kFilters, 1, editor_asset_root(), false);
}

void request_save_scene() noexcept {
  if (!world_is_editable()) {
    return;
  }
  if (editor_session().document.hasPath) {
    static_cast<void>(perform_scene_save());
    return;
  }
  editor_session().document.dialogContinuesPendingAction = false;
  begin_save_scene_as_dialog();
}

void request_save_scene_as() noexcept {
  if (!world_is_editable()) {
    return;
  }
  editor_session().document.dialogContinuesPendingAction = false;
  begin_save_scene_as_dialog();
}

void scene_document_poll_dialog_result() noexcept {
  SceneDocumentState &doc = editor_session().document;
  if (!doc.dialogResultPending.load(std::memory_order_acquire)) {
    return;
  }
  doc.dialogResultPending.store(false, std::memory_order_relaxed);

  const SceneDialogKind kind = doc.dialogPendingKind;
  doc.dialogPendingKind = SceneDialogKind::None;
  const bool accepted = doc.dialogResultAccepted;
  char path[kMaxDocumentPathLength] = {};
  std::snprintf(path, sizeof(path), "%s", doc.dialogResultPath);
  const bool continues = doc.dialogContinuesPendingAction;
  doc.dialogContinuesPendingAction = false;

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
  SceneDocumentState &doc = editor_session().document;
  if (doc.recentScenesLoaded) {
    return;
  }
  doc.recentScenesLoaded = true;
  doc.recentSceneCount = 0U;

  char path[1024] = {};
  if (!build_recent_scenes_path(path, sizeof(path))) {
    return;
  }

  char buffer[8192] = {};
  std::size_t size = 0U;
  // Any non-Ok read leaves the in-memory recent list empty; a fault here is
  // recoverable convenience data, and distinguishing it from absence is the
  // reader's job (kept for diagnosis by callers that persist authored data).
  if (core::read_whole_file(path, buffer, sizeof(buffer), &size) !=
      core::FileReadResult::Ok) {
    return;
  }

  core::JsonParser parser{};
  if (!parser.parse(buffer, size)) {
    return;
  }
  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    return;
  }
  core::JsonValue scenesValue{};
  if (!parser.get_object_field(*root, "scenes", &scenesValue) ||
      (scenesValue.type != core::JsonValue::Type::Array)) {
    return;
  }

  const std::size_t count = parser.array_size(scenesValue);
  bool anyPruned = false;
  for (std::size_t i = 0U;
       (i < count) && (doc.recentSceneCount < kMaxRecentScenes); ++i) {
    core::JsonValue element{};
    if (!parser.get_array_element(scenesValue, i, &element)) {
      continue;
    }
    char entryPath[kMaxDocumentPathLength] = {};
    if (!parser.copy_string(element, entryPath, sizeof(entryPath))) {
      continue;
    }
    std::error_code ec{};
    if (!std::filesystem::is_regular_file(entryPath, ec) || ec) {
      // Gracefully handle a moved/deleted file: drop it instead of
      // listing a recent entry the user cannot open.
      anyPruned = true;
      continue;
    }
    std::snprintf(doc.recentScenes[doc.recentSceneCount],
                  kMaxDocumentPathLength, "%s", entryPath);
    ++doc.recentSceneCount;
  }

  if (anyPruned) {
    recent_scenes_persist();
  }
}

void recent_scenes_add(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return;
  }
  recent_scenes_load_once();
  SceneDocumentState &doc = editor_session().document;

  char reordered[kMaxRecentScenes][kMaxDocumentPathLength] = {};
  std::size_t writeIndex = 0U;
  std::snprintf(reordered[writeIndex++], kMaxDocumentPathLength, "%s", path);
  for (std::size_t i = 0U;
       (i < doc.recentSceneCount) && (writeIndex < kMaxRecentScenes); ++i) {
    if (std::strcmp(doc.recentScenes[i], path) == 0) {
      continue;
    }
    std::snprintf(reordered[writeIndex++], kMaxDocumentPathLength, "%.*s",
                  static_cast<int>(kMaxDocumentPathLength - 1U),
                  doc.recentScenes[i]);
  }
  for (std::size_t i = 0U; i < writeIndex; ++i) {
    std::snprintf(doc.recentScenes[i], kMaxDocumentPathLength, "%.*s",
                  static_cast<int>(kMaxDocumentPathLength - 1U), reordered[i]);
  }
  doc.recentSceneCount = writeIndex;
  recent_scenes_persist();
}

std::size_t recent_scene_count() noexcept {
  recent_scenes_load_once();
  return editor_session().document.recentSceneCount;
}

const char *recent_scene_at(std::size_t index) noexcept {
  recent_scenes_load_once();
  const SceneDocumentState &doc = editor_session().document;
  if (index >= doc.recentSceneCount) {
    return "";
  }
  return doc.recentScenes[index];
}

void scene_document_update_window_title() noexcept {
  EditorSession &session = editor_session();
  if (session.sdlWindow == nullptr) {
    return;
  }
  char title[640] = {};
  std::snprintf(title, sizeof(title), "Engine Editor - %s%s",
               scene_document_display_name(),
               scene_document_is_dirty() ? " *" : "");
  if (std::strcmp(title, session.lastAppliedWindowTitle) == 0) {
    return;
  }
  SDL_SetWindowTitle(session.sdlWindow, title);
  std::snprintf(session.lastAppliedWindowTitle,
               sizeof(session.lastAppliedWindowTitle), "%s", title);
}

void scene_document_reset_for_world_switch() noexcept {
  reset_document_identity(editor_session().document);
}

void recent_scenes_set_directory_override_for_tests(
    const char *directory) noexcept {
  if (directory == nullptr) {
    directory = "";
  }
  std::snprintf(g_recentScenesDirectoryOverride,
               sizeof(g_recentScenesDirectoryOverride), "%s", directory);
  // A new directory invalidates the in-memory cache so the next accessor
  // reloads from the (possibly now-empty) overridden location.
  editor_session().document.recentScenesLoaded = false;
  editor_session().document.recentSceneCount = 0U;
}

} // namespace engine::editor
