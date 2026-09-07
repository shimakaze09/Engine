// Implements the material editor's state, undoable edit command, document
// dirty tracking, unsaved-change gate, and business logic declared in
// editor_material_edit.h.

#include "editor_material_edit.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "editor_session.h"
#include "engine/core/logging.h"
#include "engine/runtime/editor_bridge.h"

namespace engine::editor {

namespace {

constexpr const char *kLogChannel = "editor.material";

MaterialEditorState g_state{};
// Beside g_state rather than inside it: CommandHistory is neither copyable
// nor movable, and the state is reset by whole-value assignment.
CommandHistory g_history{};

bool vec3_equal(const math::Vec3 &lhs, const math::Vec3 &rhs) noexcept {
  return (lhs.x == rhs.x) && (lhs.y == rhs.y) && (lhs.z == rhs.z);
}

bool vec2_equal(const math::Vec2 &lhs, const math::Vec2 &rhs) noexcept {
  return (lhs.x == rhs.x) && (lhs.y == rhs.y);
}

/// Exact-value comparison: used only to detect "did this gesture actually
/// change anything" before spending an undo slot, not for tolerance-based
/// numeric reasoning.
bool params_equal(const renderer::Material &lhs,
                  const renderer::Material &rhs) noexcept {
  return vec3_equal(lhs.albedo, rhs.albedo) &&
         vec3_equal(lhs.emissive, rhs.emissive) &&
         (lhs.roughness == rhs.roughness) && (lhs.metallic == rhs.metallic) &&
         (lhs.opacity == rhs.opacity) && (lhs.alphaMode == rhs.alphaMode) &&
         (lhs.alphaCutoff == rhs.alphaCutoff) &&
         vec2_equal(lhs.uvTiling, rhs.uvTiling) &&
         vec2_equal(lhs.uvOffset, rhs.uvOffset);
}

bool slots_equal(const renderer::MaterialTextureSlots &lhs,
                 const renderer::MaterialTextureSlots &rhs) noexcept {
  return (lhs.albedo == rhs.albedo) &&
         (lhs.metallicRoughness == rhs.metallicRoughness) &&
         (lhs.emissive == rhs.emissive) && (lhs.occlusion == rhs.occlusion) &&
         (lhs.opacity == rhs.opacity);
}

/// Finalizes any in-progress gesture on the currently-open material,
/// pushing an undo step only when the buffer actually differs from the
/// gesture's starting point. Safe to call when no gesture is active.
void finalize_pending_gesture() noexcept {
  if (!g_state.gestureActive) {
    return;
  }
  g_state.gestureActive = false;

  if (params_equal(g_state.gestureBeforeParams, g_state.buffer) &&
      slots_equal(g_state.gestureBeforeSlots, g_state.textureSlots)) {
    return;
  }

  auto *cmd = new (std::nothrow) MaterialEditCommand();
  if (cmd == nullptr) {
    return;
  }
  cmd->materialId = g_state.materialId;
  cmd->before = g_state.gestureBeforeParams;
  cmd->slotsBefore = g_state.gestureBeforeSlots;
  cmd->after = g_state.buffer;
  cmd->slotsAfter = g_state.textureSlots;
  g_history.execute(cmd);
}

/// Copies a loaded bridge state into the panel state and marks the
/// document clean at the current history position.
void adopt_loaded_state(const runtime::EditorMaterialState &loaded) noexcept {
  g_state.materialId = loaded.materialId;
  g_state.buffer = loaded.params;
  g_state.textureSlots = loaded.textureSlots;
  g_state.hasParent = loaded.hasParent;
  std::snprintf(g_state.parentVirtualPath, sizeof(g_state.parentVirtualPath),
                "%s", loaded.parentVirtualPath);
  g_state.savedHistoryToken = g_history.current_token();
}

/// Loads `virtualPath` into a fresh state (history dropped): the previous
/// material, if any, is clean by the time this runs, so no command of its
/// can still matter.
void perform_open_material(const char *virtualPath) noexcept {
  finalize_pending_gesture();
  const bool wasUndoTarget = g_state.undoTarget;

  const runtime::EditorMaterialState loaded =
      runtime::editor_load_material(virtualPath);
  g_state = MaterialEditorState{};
  g_history.clear();
  g_state.open = true;
  g_state.found = loaded.found;
  g_state.undoTarget = wasUndoTarget;
  std::snprintf(g_state.virtualPath, sizeof(g_state.virtualPath), "%s",
                virtualPath);
  if (loaded.found) {
    adopt_loaded_state(loaded);
  }
}

/// Arms the unsaved-change prompt in front of `action`.
void arm_pending_action(PendingMaterialAction action,
                        const char *path) noexcept {
  g_state.pendingAction = action;
  g_state.pendingOpenPath[0] = '\0';
  if (path != nullptr) {
    std::snprintf(g_state.pendingOpenPath, sizeof(g_state.pendingOpenPath),
                  "%s", path);
  }
  g_state.unsavedPromptOpen = true;
}

/// Executes the armed action and clears the prompt; runs once the prompt
/// resolved in favor of proceeding (Save succeeded, or Discard).
void continue_pending_action() noexcept {
  const PendingMaterialAction action = g_state.pendingAction;
  char path[sizeof(g_state.pendingOpenPath)] = {};
  std::snprintf(path, sizeof(path), "%s", g_state.pendingOpenPath);
  g_state.pendingAction = PendingMaterialAction::None;
  g_state.pendingOpenPath[0] = '\0';
  g_state.unsavedPromptOpen = false;

  switch (action) {
  case PendingMaterialAction::Close:
    close_material_editor();
    break;
  case PendingMaterialAction::OpenPath:
    perform_open_material(path);
    break;
  case PendingMaterialAction::None:
  default:
    break;
  }
}

} // namespace

bool MaterialEditCommand::execute() noexcept {
  return runtime::editor_set_material_params(materialId, after, slotsAfter);
}

bool MaterialEditCommand::undo() noexcept {
  return runtime::editor_set_material_params(materialId, before, slotsBefore);
}

MaterialEditorState &material_editor_state() noexcept { return g_state; }

CommandHistory &material_editor_history() noexcept { return g_history; }

bool material_editor_is_dirty() noexcept {
  if (!g_state.open || !g_state.found) {
    return false;
  }
  return g_state.gestureActive ||
         (g_history.current_token() != g_state.savedHistoryToken);
}

void open_material_editor(const char *virtualPath) noexcept {
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    return;
  }

  if (g_state.open && (std::strcmp(g_state.virtualPath, virtualPath) == 0)) {
    // Already open on this material: keep the live buffer (do not discard
    // unsaved edits by reloading over them).
    return;
  }

  if (g_state.unsavedPromptOpen) {
    // One deferred action at a time; the prompt already on screen decides
    // the current material's fate first.
    return;
  }

  finalize_pending_gesture();
  if (material_editor_is_dirty()) {
    arm_pending_action(PendingMaterialAction::OpenPath, virtualPath);
    return;
  }
  perform_open_material(virtualPath);
}

void request_close_material_editor() noexcept {
  if (!g_state.open || g_state.unsavedPromptOpen) {
    return;
  }
  finalize_pending_gesture();
  if (material_editor_is_dirty()) {
    arm_pending_action(PendingMaterialAction::Close, nullptr);
    return;
  }
  close_material_editor();
}

void close_material_editor() noexcept {
  finalize_pending_gesture();
  g_state.open = false;
  g_state.undoTarget = false;
  g_state.unsavedPromptOpen = false;
  g_state.pendingAction = PendingMaterialAction::None;
  g_state.pendingOpenPath[0] = '\0';
  g_history.clear();
  g_state.savedHistoryToken = g_history.current_token();
}

bool material_editor_prompt_open() noexcept { return g_state.unsavedPromptOpen; }

void material_editor_prompt_choose_save() noexcept {
  if (!g_state.unsavedPromptOpen) {
    return;
  }
  if (!save_material_editor()) {
    // Save failed: lastSaveError is set for the UI; the prompt stays armed
    // so the user can retry, discard, or cancel.
    return;
  }
  continue_pending_action();
}

void material_editor_prompt_choose_discard() noexcept {
  if (!g_state.unsavedPromptOpen) {
    return;
  }
  if (!reload_material_editor_from_disk()) {
    char message[400] = {};
    std::snprintf(message, sizeof(message),
                  "discarded edits to %s could not be reverted from disk; "
                  "the live record keeps them until the file is reloaded",
                  g_state.virtualPath);
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
  }
  continue_pending_action();
}

void material_editor_prompt_choose_cancel() noexcept {
  g_state.pendingAction = PendingMaterialAction::None;
  g_state.pendingOpenPath[0] = '\0';
  g_state.unsavedPromptOpen = false;
}

/// Session-transition reset: drops all state without finalizing a gesture.
void reset_material_editor() noexcept {
  g_state = MaterialEditorState{};
  g_history.clear();
}

void material_editor_apply_frame(
    const renderer::Material &beforeFrameParams,
    const renderer::MaterialTextureSlots &beforeFrameSlots,
    bool anyFieldChangedThisFrame, bool anyItemActive) noexcept {
  if (!g_state.open || !g_state.found) {
    return;
  }

  if (anyFieldChangedThisFrame) {
    if (!g_state.gestureActive) {
      g_state.gestureActive = true;
      g_state.gestureBeforeParams = beforeFrameParams;
      g_state.gestureBeforeSlots = beforeFrameSlots;
    }
    static_cast<void>(runtime::editor_set_material_params(
        g_state.materialId, g_state.buffer, g_state.textureSlots));
  }

  if (g_state.gestureActive && !anyItemActive) {
    finalize_pending_gesture();
  }
}

bool save_material_editor() noexcept {
  if (!g_state.open || !g_state.found) {
    return false;
  }
  finalize_pending_gesture();

  const char *parent =
      g_state.hasParent ? g_state.parentVirtualPath : nullptr;
  if (!runtime::editor_save_material(g_state.virtualPath, parent)) {
    std::snprintf(g_state.lastSaveError, sizeof(g_state.lastSaveError),
                  "failed to write material %s", g_state.virtualPath);
    return false;
  }
  g_state.lastSaveError[0] = '\0';
  g_state.savedHistoryToken = g_history.current_token();
  return true;
}

bool reload_material_editor_from_disk() noexcept {
  if (!g_state.open) {
    return false;
  }
  finalize_pending_gesture();

  const runtime::EditorMaterialState reloaded =
      runtime::editor_reload_material(g_state.virtualPath);
  if (!reloaded.found) {
    return false;
  }

  adopt_loaded_state(reloaded);
  g_state.lastSaveError[0] = '\0';
  return true;
}

} // namespace engine::editor
