// Declares the editor's scene-document identity, dirty-state tracking, and
// file-operation state machine (New/Open/Save/Save As, recent scenes,
// unsaved-change gating, async native file dialogs) used by the main menu
// panel and the runtime quit bridge (issue #158).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace engine::editor {

constexpr std::size_t kMaxDocumentPathLength = 512U;
constexpr std::size_t kMaxDocumentDisplayNameLength = 128U;
constexpr std::size_t kMaxRecentScenes = 10U;

/// Enumerates a document action deferred behind the unsaved-change prompt.
enum class PendingSceneAction : std::uint8_t { None, New, OpenPath, Quit };

/// Enumerates the outstanding native file dialog kind, if any.
enum class SceneDialogKind : std::uint8_t { None, Open, SaveAs };

/// Owns scene-document identity, dirty bookkeeping, the unsaved-change
/// confirm prompt, the recent-scenes list, and the async native file
/// dialog handoff. Embedded in EditorSession. The dialog result fields are
/// written from the SDL dialog callback (same thread as the caller on
/// Windows/macOS backends, a portal worker thread on some Linux desktop
/// backends) and consumed once per frame on the main thread; the
/// std::atomic acquire/release pair on dialogResultPending is the only
/// cross-thread contract; every other field it guards is written before
/// the release store and read only after the acquire load succeeds.
struct SceneDocumentState final {
  char path[kMaxDocumentPathLength] = {};
  bool hasPath = false;
  char displayName[kMaxDocumentDisplayNameLength] = "Untitled Scene";
  std::uint64_t savedHistoryToken = 0U;

  bool unsavedPromptOpen = false;
  PendingSceneAction pendingAction = PendingSceneAction::None;
  char pendingOpenPath[kMaxDocumentPathLength] = {};

  std::atomic<bool> dialogResultPending{false};
  SceneDialogKind dialogPendingKind = SceneDialogKind::None;
  // True when the outstanding SaveAs dialog exists only to satisfy a
  // pendingAction continuation armed from the unsaved-change prompt
  // (untitled document, user chose Save); false for a direct File > Save
  // As with no follow-up action.
  bool dialogContinuesPendingAction = false;
  char dialogResultPath[kMaxDocumentPathLength] = {};
  bool dialogResultAccepted = false;

  char recentScenes[kMaxRecentScenes][kMaxDocumentPathLength] = {};
  std::size_t recentSceneCount = 0U;
  bool recentScenesLoaded = false;
  // Latched when the persisted list exists but could not be read (a read
  // fault or a file past the fixed buffer): the in-memory list starts
  // empty for the session, and automatic persistence is refused so the
  // unread bytes are never replaced by that empty or reduced list. An
  // absent file is not a fault and never sets this.
  bool recentScenesLoadFailed = false;

  char lastSaveError[kMaxDocumentPathLength + 64U] = {};
};

/// Returns the document's display name ("Untitled Scene" when unsaved).
const char *scene_document_display_name() noexcept;
/// Returns the document's absolute path, or "" when it has none yet.
const char *scene_document_path() noexcept;
/// True once the document has been saved to or opened from a path.
bool scene_document_has_path() noexcept;
/// True when the command-history position has moved since the last save
/// (or the saved position was evicted by history growth).
bool scene_document_is_dirty() noexcept;
/// Most recent save/open/save-as failure message ("" when none pending).
const char *scene_document_last_error() noexcept;

/// Resets the world to an empty scene and clears document identity.
/// Callers must have already resolved unsaved changes; use
/// request_scene_new for the gated entry point. False when the world is
/// unbound or not currently editable (bound, stopped, Input phase).
bool perform_scene_new() noexcept;
/// Loads the scene file at `path` into the attached world and adopts it
/// as the document identity on success. The previous world/document/
/// history are left completely untouched on failure (load_scene is
/// transactional). Callers must have already resolved unsaved changes;
/// use request_scene_open for the gated entry point.
bool perform_scene_open(const char *path) noexcept;
/// Saves to the current document path; false when the document has no
/// path yet, the world is not editable, or the atomic write failed (the
/// previous file and dirty status are both left untouched on failure).
bool perform_scene_save() noexcept;
/// Saves to `path` after validating it resolves inside the editor asset
/// root, then adopts it as the document identity on success. The
/// previous document identity and dirty status are untouched on failure.
bool perform_scene_save_as(const char *path) noexcept;

/// True when `path`'s parent directory resolves inside `root` — the
/// stand-in project jail until #137 lands true project roots. Pure/
/// testable core behind scene_path_passes_jail().
bool scene_path_passes_jail_under(const char *path, const char *root) noexcept;
/// scene_path_passes_jail_under(path, editor_asset_root()).
bool scene_path_passes_jail(const char *path) noexcept;

/// Gated entry points: perform immediately when the document is clean;
/// otherwise arm the unsaved-change prompt and defer.
void request_scene_new() noexcept;
void request_scene_open(const char *path) noexcept;
/// True when the caller may proceed with an immediate quit (document was
/// clean); false means the prompt was armed and the caller must not quit
/// until the prompt resolves the pending PendingSceneAction::Quit.
bool request_scene_quit() noexcept;

/// True while the unsaved-change confirm prompt should be drawn.
bool scene_document_prompt_open() noexcept;
/// User chose Save from the confirm prompt (saves in place, or defers to
/// a Save As dialog for an untitled document; the prompt stays armed on
/// a save failure so the user can retry or cancel).
void scene_document_prompt_choose_save() noexcept;
/// User chose Discard: proceeds with the pending action unsaved.
void scene_document_prompt_choose_discard() noexcept;
/// User chose Cancel: drops the pending action, no side effects.
void scene_document_prompt_choose_cancel() noexcept;

/// Begins a direct (non-gated) File > Open Scene native dialog; its
/// result re-applies the unsaved-change gate through request_scene_open.
void request_open_scene_dialog() noexcept;
/// File > Save: saves in place, or begins a Save As dialog when untitled.
void request_save_scene() noexcept;
/// File > Save As: always begins a native save dialog.
void request_save_scene_as() noexcept;
/// Polls the async dialog result once per frame; must run on the main
/// thread. No-op when no result is pending.
void scene_document_poll_dialog_result() noexcept;

/// Recent-scenes list: MRU-ordered, persisted to the platform save
/// directory, pruned of unreadable entries on load and on failed opens.
void recent_scenes_load_once() noexcept;
void recent_scenes_add(const char *path) noexcept;
std::size_t recent_scene_count() noexcept;
const char *recent_scene_at(std::size_t index) noexcept;

/// Refreshes the OS window title from the document identity/dirty state;
/// a no-op past the first call in a frame where nothing changed.
void scene_document_update_window_title() noexcept;

/// Clears document identity back to "Untitled Scene"/clean; called by
/// editor_set_world when the bound World* changes, since a different
/// World can never still be the file that was loaded into the old one.
/// The recent-scenes cache and any in-flight dialog are session-lifetime
/// and are not touched.
void scene_document_reset_for_world_switch() noexcept;

/// Test-only override for the recent-scenes persistence directory; an
/// empty string restores the default per-user platform save directory.
/// Exists so tests never read or write the real user's save directory.
void recent_scenes_set_directory_override_for_tests(
    const char *directory) noexcept;

} // namespace engine::editor
