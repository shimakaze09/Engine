// Declares the editor's action table: every command a key chord or a menu
// item can run, its menu label, its chord, and whether it repeats while
// held. Key dispatch, the menus and the context menus all read this one
// table, so a label cannot advertise a chord the dispatcher does not
// honour, and a chord matches only with exactly its modifiers.

#pragma once

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <cstddef>
#include <cstdint>

namespace engine::editor {

/// Every editor command reachable from a shortcut or a menu.
enum class EditorAction : std::uint8_t {
  NewScene,
  OpenScene,
  SaveScene,
  SaveSceneAs,
  Exit,
  Undo,
  Redo,
  Copy,
  Paste,
  PasteAsChild,
  Duplicate,
  Delete,
  CreateEmpty,
  GizmoTranslate,
  GizmoRotate,
  GizmoScale,
  PlayStop,
  Pause,
  Step,
  Count,
};

/// One row of the action table.
struct EditorShortcut final {
  EditorAction action = EditorAction::Count;
  /// Stable identifier, the name a stored binding refers to.
  const char *id = nullptr;
  /// Menu text.
  const char *label = nullptr;
  /// ImGuiKey plus ImGuiMod_* flags; 0 when the action has no chord.
  ImGuiKeyChord chord = 0;
  /// A second chord for the same action (Ctrl+Y beside Ctrl+Shift+Z);
  /// 0 when none.
  ImGuiKeyChord alternate = 0;
  /// Whether holding the chord runs the action again at the key-repeat
  /// rate. Only actions that are safe to run many times repeat; one that
  /// creates or writes something runs once per press.
  bool repeats = false;
  /// Whether the chord still fires while the game has the keyboard (the
  /// Game view focused in play). Only the play controls do, so the author
  /// can always pause or stop a running game from the keyboard.
  bool whileGameHasKeyboard = false;
};

/// Number of rows, one per EditorAction.
std::size_t editor_shortcut_count() noexcept;
/// The row at `index` (< editor_shortcut_count()).
const EditorShortcut &editor_shortcut_at(std::size_t index) noexcept;
/// The row for `action`.
const EditorShortcut &editor_shortcut(EditorAction action) noexcept;

/// True when `action` can run now: the menu's enabled state and the
/// dispatcher's precondition are the same test.
bool editor_action_enabled(EditorAction action) noexcept;
/// Runs `action` when it is enabled. True when it ran.
bool run_editor_action(EditorAction action) noexcept;

/// The chord as menu text ("Ctrl+Shift+S"); "" for no chord. The pointer
/// stays valid until the table's chords change.
const char *editor_shortcut_text(EditorAction action) noexcept;

/// True while no shortcut may fire: a text field has the keyboard, or a
/// popup or the unsaved-changes prompt is open. While the game has the
/// keyboard only rows marked whileGameHasKeyboard fire.
bool editor_shortcuts_blocked() noexcept;
/// Runs every action whose chord was pressed this frame, unless blocked.
/// Call once per frame, after ImGui::NewFrame.
void dispatch_editor_shortcuts() noexcept;

/// Draws the menu item for `action` with its label, chord and enabled
/// state (checked when `checked`), and runs it when clicked. True when it
/// ran.
bool editor_action_menu_item(EditorAction action,
                             bool checked = false) noexcept;
/// Draws the same menu item without running it: true when it was clicked
/// while enabled. For callers that must apply the action later, such as a
/// row inside a hierarchy walk.
bool editor_action_menu_item_clicked(EditorAction action,
                                     bool checked = false) noexcept;

} // namespace engine::editor
