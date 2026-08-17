// Implements the material editor's state, undoable edit command, and
// business logic declared in editor_material_edit.h.

#include "editor_material_edit.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "editor_session.h"
#include "engine/runtime/editor_bridge.h"

namespace engine::editor {

namespace {

MaterialEditorState g_state{};

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
  editor_session().commandHistory.execute(cmd);
}

} // namespace

bool MaterialEditCommand::execute() noexcept {
  return runtime::editor_set_material_params(materialId, after, slotsAfter);
}

bool MaterialEditCommand::undo() noexcept {
  return runtime::editor_set_material_params(materialId, before, slotsBefore);
}

MaterialEditorState &material_editor_state() noexcept { return g_state; }

void open_material_editor(const char *virtualPath) noexcept {
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    return;
  }

  if (g_state.open && (std::strcmp(g_state.virtualPath, virtualPath) == 0)) {
    // Already open on this material: keep the live buffer (do not discard
    // unsaved edits by reloading over them).
    return;
  }

  finalize_pending_gesture();

  const runtime::EditorMaterialState loaded =
      runtime::editor_load_material(virtualPath);
  g_state = MaterialEditorState{};
  g_state.open = true;
  g_state.found = loaded.found;
  std::snprintf(g_state.virtualPath, sizeof(g_state.virtualPath), "%s",
               virtualPath);
  if (loaded.found) {
    g_state.materialId = loaded.materialId;
    g_state.buffer = loaded.params;
    g_state.textureSlots = loaded.textureSlots;
    g_state.hasParent = loaded.hasParent;
    std::snprintf(g_state.parentVirtualPath, sizeof(g_state.parentVirtualPath),
                 "%s", loaded.parentVirtualPath);
  }
}

void close_material_editor() noexcept {
  finalize_pending_gesture();
  g_state.open = false;
}

/// Session-transition reset: drops all state without finalizing a gesture.
void reset_material_editor() noexcept { g_state = MaterialEditorState{}; }

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
    g_state.dirty = true;
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
    return false;
  }
  g_state.dirty = false;
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

  g_state.materialId = reloaded.materialId;
  g_state.buffer = reloaded.params;
  g_state.textureSlots = reloaded.textureSlots;
  g_state.hasParent = reloaded.hasParent;
  std::snprintf(g_state.parentVirtualPath, sizeof(g_state.parentVirtualPath),
               "%s", reloaded.parentVirtualPath);
  g_state.dirty = false;
  return true;
}

} // namespace engine::editor
