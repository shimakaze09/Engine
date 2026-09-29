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
  CreateEmptyChild,
  Rename,
  GizmoTranslate,
  GizmoRotate,
  GizmoScale,
  GizmoSpace,
  FrameSelected,
  PlayStop,
  Pause,
  Step,
  Screenshot,
  RecordPlay,
  ReplayLatest,
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
  /// can always pause or stop a running game from the keyboard, and Take
  /// Screenshot, as Unreal's F9 works in Play In Editor.
  bool whileGameHasKeyboard = false;
  /// Whether the action edits the scene's entities (Copy, Paste,
  /// Duplicate, Delete, Create Empty, Rename, Frame Selected), so its
  /// chord and the Edit menu's item work only while the Scene view or the
  /// Entities panel is the last focused panel, as Unity routes them to the
  /// focused window. Everything else works from anywhere.
  bool sceneEditing = false;
};

/// Number of rows, one per EditorAction. Rows carry the live bindings,
/// the defaults below with any rebinding applied.
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
/// The label `action` shows now, in a menu or on a button: the row's own
/// label, except that PlayStop reads "Stop" while a session runs, as
/// Unity's Play toggle and UE5's play toolbar do.
const char *editor_action_label(EditorAction action) noexcept;

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

/// Writes `chord` as saved text ("Ctrl+Shift+S", or "None" for no chord);
/// false for a modifier alone or a key a binding cannot use.
bool format_key_chord(ImGuiKeyChord chord, char *out,
                      std::size_t capacity) noexcept;
/// Reads text format_key_chord writes; false for anything else.
bool parse_key_chord(const char *text, ImGuiKeyChord *out) noexcept;
/// The action whose stable id is `id`.
bool find_editor_action(const char *id, EditorAction *out) noexcept;

/// Binds `chord` (0 unbinds) as `action`'s primary chord. Refused when
/// another action already uses it, naming that action in *outConflict,
/// or when the chord cannot be named.
bool rebind_editor_action(EditorAction action, ImGuiKeyChord chord,
                          EditorAction *outConflict) noexcept;
/// True when `action`'s chords differ from its defaults.
bool editor_action_rebound(EditorAction action) noexcept;
/// Restores every default chord.
void reset_editor_shortcuts() noexcept;

/// Loads the bindings the preferences section stores, following the
/// settings reader's phases: begin clears what was staged, stage records
/// one stored line, and commit replaces every binding with the defaults
/// plus the staged lines. Conflicts are judged on that final table, so a
/// line may take a chord a later line frees. An unknown id or a malformed
/// chord is refused when staged, and a chord two actions would share when
/// committed, each with a logged warning; a refused action keeps its
/// default.
void begin_stored_shortcuts() noexcept;
bool stage_stored_shortcut(const char *id, const char *chordText) noexcept;
void commit_stored_shortcuts() noexcept;

/// Starts capturing the next chord for `action` in Preferences; shortcuts
/// do not dispatch meanwhile.
void begin_shortcut_capture(EditorAction action) noexcept;
/// The action being captured, or EditorAction::Count.
EditorAction shortcut_capture_target() noexcept;
/// Stops capturing.
void end_shortcut_capture() noexcept;

/// Notes which docked panel has focus in editor_session().lastFocusedPanel.
/// A focused menu, popup, toolbar or other window leaves it as it was.
/// dispatch_editor_shortcuts calls it first.
void update_focused_panel() noexcept;

/// True when `action` may act from the panel that last had focus: always,
/// except for a scene-editing row, which needs the Scene view or the
/// Entities panel.
bool editor_action_in_focus_scope(EditorAction action) noexcept;

/// The Edit menu's form of editor_action_menu_item: also disabled while
/// the action is out of focus scope, since the menu acts on the panel the
/// author was working in. A panel's own context menu uses
/// editor_action_menu_item, being in scope by where it opened.
bool editor_edit_menu_item(EditorAction action, bool checked = false) noexcept;

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
