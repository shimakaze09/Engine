// Declares the material editor's state, undoable edit command, and
// business-logic entry points (open/close/save/reload/live-apply). Drawing
// lives in editor_panels_material.{h,cpp}; this module has no ImGui
// dependency so its command/gesture logic stays headless-testable.

#pragma once

#include "engine/editor/command_history.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material.h"

namespace engine::editor {

/// The material editor panel's full editable state: which material is
/// open, its live-editing buffer, the parent path, and in-progress gesture
/// bracketing (so a slider drag pushes exactly one undo step, not one per
/// frame, mirroring the Inspector's per-gesture undo granularity through a
/// simpler activate/deactivate bracket instead of its cross-frame merge).
struct MaterialEditorState final {
  bool open = false;
  bool found = false;
  char virtualPath[260] = {};
  renderer::AssetId materialId = renderer::kInvalidAssetId;
  renderer::Material buffer{};
  renderer::MaterialTextureSlots textureSlots{};
  char parentVirtualPath[260] = {};
  bool hasParent = false;

  bool dirty = false; // live edits not yet saved to disk

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

// Discards the whole material editor state — open panel, buffer, asset id,
// and any in-progress gesture — without pushing history. Called on world
// rebind and editor shutdown, where the referenced asset service is going
// away (#168); user-driven close goes through close_material_editor.
void reset_material_editor() noexcept;

/// Opens the panel for `virtualPath` (loading it if not already loaded). A
/// no-op re-open of the already-open material keeps the current buffer
/// (does not discard unsaved edits); switching to a different material
/// first finalizes any gesture still active on the previous one.
void open_material_editor(const char *virtualPath) noexcept;
/// Closes the panel without discarding any already-applied live edits
/// (they stay live in the database until Save or a scene/process
/// restart) -- only the panel's own visibility state changes.
void close_material_editor() noexcept;

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
/// layer); the previous file on disk is guaranteed untouched.
bool save_material_editor() noexcept;

/// Re-reads the file from disk, discarding any unsaved live edits; a
/// malformed file leaves the current buffer and the live database record
/// both untouched (reload_material_asset's contract) and returns false.
bool reload_material_editor_from_disk() noexcept;

} // namespace engine::editor
