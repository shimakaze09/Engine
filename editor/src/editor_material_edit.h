// Declares the material editor's state, undoable edit command, and
// business-logic entry points (open/close/save/reload/live-apply plus the
// unsaved-change gate). The open material is its own document: it owns its
// undo history and its dirty state independently of the scene document, so
// a scene save can never clear a material's unsaved edits and the quit gate
// can account for both. Drawing lives in editor_panels_material.{h,cpp};
// this module has no ImGui dependency so its command/gesture logic stays
// headless-testable.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/editor/command_history.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material.h"

namespace engine::editor {

/// Enumerates a material-editor action deferred behind its unsaved-change
/// prompt: closing the panel, or opening a different material in it.
enum class PendingMaterialAction : std::uint8_t { None, Close, OpenPath };

/// The material editor panel's full editable state: which material is
/// open, its live-editing buffer, the parent path, dirty bookkeeping, the
/// unsaved-change prompt, and in-progress gesture bracketing (so a slider
/// drag pushes exactly one undo step, not one per frame, mirroring the
/// Inspector's per-gesture undo granularity through a simpler activate/
/// deactivate bracket instead of its cross-frame merge).
struct MaterialEditorState final {
  bool open = false;
  bool found = false;
  char virtualPath[260] = {};
  renderer::AssetId materialId = renderer::kInvalidAssetId;
  renderer::Material buffer{};
  renderer::MaterialTextureSlots textureSlots{};
  char parentVirtualPath[260] = {};
  bool hasParent = false;

  /// material_editor_history() position the file on disk matches; the
  /// document is dirty while the history cursor sits elsewhere (undo past
  /// a save re-dirties, redo back to it re-cleans) or a gesture is live.
  std::uint64_t savedHistoryToken = 0U;

  /// True while the panel is the undo target: it was the last regular
  /// window focused (menus and popups do not steal the target, so Edit >
  /// Undo reaches the material the user was just editing). Owned by the
  /// panel's draw; cleared with the rest of the state.
  bool undoTarget = false;

  bool unsavedPromptOpen = false;
  PendingMaterialAction pendingAction = PendingMaterialAction::None;
  char pendingOpenPath[260] = {};
  char lastSaveError[320] = {};

  bool gestureActive = false;
  renderer::Material gestureBeforeParams{};
  renderer::MaterialTextureSlots gestureBeforeSlots{};
};

/// Undoable material param/texture-slot edit: execute/undo both write
/// straight into the live asset database record (editor_set_material_
/// params) -- the same viewport-preview mutation a live drag already
/// applies, so redo/undo is instant with no disk round trip.
struct MaterialEditCommand final : EditorCommand {
  renderer::AssetId materialId = renderer::kInvalidAssetId;
  renderer::Material before{};
  renderer::MaterialTextureSlots slotsBefore{};
  renderer::Material after{};
  renderer::MaterialTextureSlots slotsAfter{};

  bool execute() noexcept override;
  bool undo() noexcept override;
};

/// Returns the process-wide material editor panel state.
MaterialEditorState &material_editor_state() noexcept;

/// Returns the material document's own undo history. Material commands
/// never enter the scene's history: the two documents persist separately,
/// so their dirty positions must be tracked separately too. Cleared when
/// the material closes or switches (the record is clean by then, so no
/// command can outlive the document it edited) and on every reset.
CommandHistory &material_editor_history() noexcept;

/// True when the open material's live record differs from its file: a
/// gesture is in flight, or the history cursor is not at the saved
/// position. Never true while no material is open.
bool material_editor_is_dirty() noexcept;

// Discards the whole material editor state — open panel, buffer, asset id,
// undo history, and any in-progress gesture — without pushing history.
// Called on world rebind and editor shutdown, where the referenced asset
// service is going away (#168); user-driven close goes through
// request_close_material_editor.
void reset_material_editor() noexcept;

/// Opens the panel for `virtualPath` (loading it if not already loaded). A
/// no-op re-open of the already-open material keeps the current buffer
/// (does not discard unsaved edits). Switching away from a dirty material
/// arms the unsaved-change prompt and defers the open until it resolves;
/// switching from a clean one first finalizes any gesture still active on
/// it and drops its history.
void open_material_editor(const char *virtualPath) noexcept;
/// Gated close: closes immediately when the material is clean, otherwise
/// arms the unsaved-change prompt and defers the close.
void request_close_material_editor() noexcept;
/// Ungated close: hides the panel and drops the material's undo history
/// without touching the live record. Callers must have already resolved
/// unsaved changes (the prompt does, before it continues here); an edit
/// still live in the record at this point stays live until a reload or a
/// process restart, with no document left tracking it.
void close_material_editor() noexcept;

/// True while the material unsaved-change prompt should be drawn.
bool material_editor_prompt_open() noexcept;
/// User chose Save: persists the material, then continues the deferred
/// action; on a save failure the prompt stays armed (lastSaveError set)
/// so the user can retry, discard, or cancel.
void material_editor_prompt_choose_save() noexcept;
/// User chose Discard: reverts the live record to the file on disk
/// (reload), then continues the deferred action. When the file cannot be
/// re-read the live edits stay in the record (nothing on disk is touched)
/// and the action still continues, since the user chose to abandon them.
void material_editor_prompt_choose_discard() noexcept;
/// User chose Cancel: drops the deferred action; the material stays open
/// and dirty exactly as it was.
void material_editor_prompt_choose_cancel() noexcept;

/// Applies `state.buffer`/`state.textureSlots` live (viewport-visible
/// immediately, every frame something changed) and, once the current
/// interaction ends, pushes exactly one undoable MaterialEditCommand for
/// the whole gesture. The caller (editor_panels_material.cpp) snapshots
/// `state.buffer`/`state.textureSlots` before drawing any widget this
/// frame and passes that snapshot as `beforeFrameParams`/
/// `beforeFrameSlots` -- the gesture's recorded starting point the first
/// time a frame reports a change. `anyFieldChangedThisFrame`: whether any
/// widget in the panel returned true this frame. `anyItemActive`: whether
/// any widget in the panel is still being interacted with. All four are
/// owned by the caller so this module stays ImGui-free.
void material_editor_apply_frame(
    const renderer::Material &beforeFrameParams,
    const renderer::MaterialTextureSlots &beforeFrameSlots,
    bool anyFieldChangedThisFrame, bool anyItemActive) noexcept;

/// Persists the current buffer to disk (staged atomic write); marks the
/// state clean on success. False on failure (logged by the bridge/writer
/// layer; lastSaveError names the material); the previous file on disk is
/// guaranteed untouched.
bool save_material_editor() noexcept;

/// Re-reads the file from disk, discarding any unsaved live edits; a
/// malformed file leaves the current buffer and the live database record
/// both untouched (reload_material_asset's contract) and returns false.
bool reload_material_editor_from_disk() noexcept;

} // namespace engine::editor
