// Implements the editor's action table and its key dispatch. Chords are
// matched exactly, modifiers included, the way Unity, Unreal and Godot
// bind them: Ctrl+S never fires for Ctrl+Shift+S, and a bare W never fires
// with Ctrl held. Actions that create or write something run once per
// press; only undo and redo repeat while held, as in those editors.

#include "editor_shortcuts.h"

#include "editor_commands.h"
#include "editor_scene_document.h"
#include "editor_session.h"

#include "ImGuizmo.h"

#include <array>
#include <cstdio>

namespace engine::editor {

namespace {

constexpr std::array<EditorShortcut,
                     static_cast<std::size_t>(EditorAction::Count)>
    kShortcuts = {{
        {EditorAction::NewScene, "file.new_scene", "New Scene",
         ImGuiMod_Ctrl | ImGuiKey_N, 0, false},
        {EditorAction::OpenScene, "file.open_scene", "Open Scene...",
         ImGuiMod_Ctrl | ImGuiKey_O, 0, false},
        {EditorAction::SaveScene, "file.save", "Save",
         ImGuiMod_Ctrl | ImGuiKey_S, 0, false},
        {EditorAction::SaveSceneAs, "file.save_as", "Save As...",
         ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, 0, false},
        {EditorAction::Undo, "edit.undo", "Undo", ImGuiMod_Ctrl | ImGuiKey_Z, 0,
         true},
        {EditorAction::Redo, "edit.redo", "Redo",
         ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
         ImGuiMod_Ctrl | ImGuiKey_Y, true},
        {EditorAction::Duplicate, "edit.duplicate", "Duplicate",
         ImGuiMod_Ctrl | ImGuiKey_D, 0, false},
        {EditorAction::GizmoTranslate, "tools.translate", "Move", ImGuiKey_W, 0,
         false},
        {EditorAction::GizmoRotate, "tools.rotate", "Rotate", ImGuiKey_E, 0,
         false},
        {EditorAction::GizmoScale, "tools.scale", "Scale", ImGuiKey_R, 0,
         false},
    }};

// Each row sits at its action's index, so a lookup is an index.
constexpr bool rows_follow_action_order() noexcept {
  for (std::size_t i = 0U; i < kShortcuts.size(); ++i) {
    if (static_cast<std::size_t>(kShortcuts[i].action) != i) {
      return false;
    }
  }
  return true;
}
static_assert(rows_follow_action_order(),
              "kShortcuts rows must follow EditorAction order");

/// Menu text per row, formatted on first use.
std::array<std::array<char, 32>, kShortcuts.size()> g_chordText{};
bool g_chordTextBuilt = false;

void format_chord(ImGuiKeyChord chord, char *out, std::size_t capacity) {
  out[0] = '\0';
  if (chord == 0) {
    return;
  }
  const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
  const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
  std::snprintf(out, capacity, "%s%s%s%s%s",
                ((chord & ImGuiMod_Ctrl) != 0) ? (mac ? "Cmd+" : "Ctrl+") : "",
                ((chord & ImGuiMod_Shift) != 0) ? "Shift+" : "",
                ((chord & ImGuiMod_Alt) != 0) ? (mac ? "Option+" : "Alt+") : "",
                ((chord & ImGuiMod_Super) != 0) ? (mac ? "Ctrl+" : "Super+")
                                                : "",
                ImGui::GetKeyName(key));
}

/// Exact match: the held modifiers are the chord's, no more and no fewer,
/// and the key went down this frame (or repeated, when the row repeats).
bool chord_pressed(ImGuiKeyChord chord, bool repeats) noexcept {
  if (chord == 0) {
    return false;
  }
  const ImGuiKeyChord mods = chord & ImGuiMod_Mask_;
  const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
  return (ImGui::GetIO().KeyMods == mods) && ImGui::IsKeyPressed(key, repeats);
}

bool has_selection() noexcept {
  return selected_entity() != runtime::kInvalidEntity;
}

} // namespace

std::size_t editor_shortcut_count() noexcept { return kShortcuts.size(); }

const EditorShortcut &editor_shortcut_at(std::size_t index) noexcept {
  return kShortcuts[(index < kShortcuts.size()) ? index : 0U];
}

const EditorShortcut &editor_shortcut(EditorAction action) noexcept {
  return editor_shortcut_at(static_cast<std::size_t>(action));
}

bool editor_action_enabled(EditorAction action) noexcept {
  switch (action) {
  case EditorAction::NewScene:
  case EditorAction::OpenScene:
  case EditorAction::SaveSceneAs:
    // Available after a failed Stop restore: they are its recovery path.
    return world_can_load_scene();
  case EditorAction::SaveScene:
    return world_is_editable();
  case EditorAction::Undo:
    return editor_history_can_undo();
  case EditorAction::Redo:
    return editor_history_can_redo();
  case EditorAction::Duplicate:
    return world_is_editable() && has_selection();
  case EditorAction::GizmoTranslate:
  case EditorAction::GizmoRotate:
  case EditorAction::GizmoScale:
    return true;
  case EditorAction::Count:
  default:
    return false;
  }
}

bool run_editor_action(EditorAction action) noexcept {
  if (!editor_action_enabled(action)) {
    return false;
  }
  switch (action) {
  case EditorAction::NewScene:
    request_scene_new();
    return true;
  case EditorAction::OpenScene:
    request_open_scene_dialog();
    return true;
  case EditorAction::SaveScene:
    request_save_scene();
    return true;
  case EditorAction::SaveSceneAs:
    request_save_scene_as();
    return true;
  case EditorAction::Undo:
    editor_history_undo();
    return true;
  case EditorAction::Redo:
    editor_history_redo();
    return true;
  case EditorAction::Duplicate: {
    const runtime::Entity copy = execute_entity_duplicate(selected_entity());
    if (copy == runtime::kInvalidEntity) {
      return false;
    }
    select_entity(copy, false);
    return true;
  }
  case EditorAction::GizmoTranslate:
    editor_session().gizmoOp = ImGuizmo::TRANSLATE;
    return true;
  case EditorAction::GizmoRotate:
    editor_session().gizmoOp = ImGuizmo::ROTATE;
    return true;
  case EditorAction::GizmoScale:
    editor_session().gizmoOp = ImGuizmo::SCALE;
    return true;
  case EditorAction::Count:
  default:
    return false;
  }
}

const char *editor_shortcut_text(EditorAction action) noexcept {
  if (!g_chordTextBuilt) {
    for (std::size_t i = 0U; i < kShortcuts.size(); ++i) {
      format_chord(kShortcuts[i].chord, g_chordText[i].data(),
                   g_chordText[i].size());
    }
    g_chordTextBuilt = true;
  }
  const auto index = static_cast<std::size_t>(action);
  return (index < g_chordText.size()) ? g_chordText[index].data() : "";
}

bool editor_shortcuts_blocked() noexcept {
  const ImGuiIO &io = ImGui::GetIO();
  // A popup (a menu, a context menu, a combo, a modal) takes the keyboard
  // while open, and the unsaved-changes prompt counts from the moment it
  // is armed, before its modal is drawn.
  return io.WantTextInput || game_owns_keyboard() ||
         scene_document_prompt_open() ||
         ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel);
}

void dispatch_editor_shortcuts() noexcept {
  if (editor_shortcuts_blocked()) {
    return;
  }
  for (const EditorShortcut &row : kShortcuts) {
    if (chord_pressed(row.chord, row.repeats) ||
        chord_pressed(row.alternate, row.repeats)) {
      static_cast<void>(run_editor_action(row.action));
    }
  }
}

bool editor_action_menu_item(EditorAction action) noexcept {
  const EditorShortcut &row = editor_shortcut(action);
  if (!ImGui::MenuItem(row.label, editor_shortcut_text(action), false,
                       editor_action_enabled(action))) {
    return false;
  }
  return run_editor_action(action);
}

} // namespace engine::editor
