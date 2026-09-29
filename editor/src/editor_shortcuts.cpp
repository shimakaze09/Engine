// Implements the editor's action table and its key dispatch. Chords are
// matched exactly, modifiers included, the way Unity, Unreal and Godot
// bind them: Ctrl+S never fires for Ctrl+Shift+S, and a bare W never fires
// with Ctrl held. Actions that create or write something run once per
// press; only undo and redo repeat while held, as in those editors. While
// the game has the keyboard only the play controls and Take Screenshot fire,
// and the entity edits fire only while the Scene view or the Entities panel
// is the panel last focused, as Unity routes them to the focused window.

#include "editor_shortcuts.h"

#include "editor_commands.h"
#include "editor_entity_clipboard.h"
#include "editor_frame_selection.h"
#include "editor_play_recording.h"
#include "editor_scene_document.h"
#include "editor_screenshot.h"
#include "editor_session.h"

#include "ImGuizmo.h"
#include "imgui_internal.h"

#include "engine/core/logging.h"
#include "engine/core/platform.h"

#include <array>
#include <cstdio>
#include <cstring>

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
        // No chord: the window manager's own close (Alt+F4, Cmd+Q) reaches
        // the same quit guard.
        {EditorAction::Exit, "file.exit", "Exit", 0, 0, false},
        {EditorAction::Undo, "edit.undo", "Undo", ImGuiMod_Ctrl | ImGuiKey_Z, 0,
         true},
        {EditorAction::Redo, "edit.redo", "Redo",
         ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
         ImGuiMod_Ctrl | ImGuiKey_Y, true},
        // The entity edits act where the author is editing entities: the
        // Scene view or the Entities panel.
        {EditorAction::Copy, "edit.copy", "Copy", ImGuiMod_Ctrl | ImGuiKey_C, 0,
         false, false, true},
        {EditorAction::Paste, "edit.paste", "Paste", ImGuiMod_Ctrl | ImGuiKey_V,
         0, false, false, true},
        {EditorAction::PasteAsChild, "edit.paste_as_child", "Paste As Child",
         ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V, 0, false, false, true},
        {EditorAction::Duplicate, "edit.duplicate", "Duplicate",
         ImGuiMod_Ctrl | ImGuiKey_D, 0, false, false, true},
        // Cmd+Backspace is the Mac chord (Ctrl maps to Cmd there), for
        // keyboards without a forward-delete key.
        {EditorAction::Delete, "edit.delete", "Delete", ImGuiKey_Delete,
         ImGuiMod_Ctrl | ImGuiKey_Backspace, false, false, true},
        {EditorAction::CreateEmpty, "entity.create_empty", "Create Empty",
         ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N, 0, false, false, true},
        {EditorAction::GizmoTranslate, "tools.translate", "Move", ImGuiKey_W, 0,
         false},
        {EditorAction::GizmoRotate, "tools.rotate", "Rotate", ImGuiKey_E, 0,
         false},
        {EditorAction::GizmoScale, "tools.scale", "Scale", ImGuiKey_R, 0,
         false},
        // Unity's chord for its handle-rotation toggle.
        {EditorAction::GizmoSpace, "tools.toggle_space", "World/Local Axes",
         ImGuiKey_X, 0, false},
        {EditorAction::FrameSelected, "view.frame_selected", "Frame Selected",
         ImGuiKey_F, 0, false, false, true},
        // Unity's play chords, live while the game has the keyboard so a
        // running game can always be paused or stopped.
        {EditorAction::PlayStop, "play.play_stop", "Play",
         ImGuiMod_Ctrl | ImGuiKey_P, 0, false, true},
        {EditorAction::Pause, "play.pause", "Pause",
         ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, 0, false, true},
        {EditorAction::Step, "play.step", "Step",
         ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_P, 0, false, true},
        // Unreal's screenshot key, live in play for the same reason.
        {EditorAction::Screenshot, "view.screenshot", "Take Screenshot",
         ImGuiKey_F9, 0, false, true},
        // Unreal's demorec and demoplay have no default chords either.
        {EditorAction::RecordPlay, "play.record", "Record Play", 0, 0, false},
        {EditorAction::ReplayLatest, "play.replay_latest",
         "Replay Latest Recording", 0, 0, false},
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

/// The live bindings: the defaults above, with any rebinding applied.
std::array<EditorShortcut, kShortcuts.size()> g_rows = kShortcuts;

/// Menu text per row, rebuilt when a binding changes.
std::array<std::array<char, 40>, kShortcuts.size()> g_chordText{};
bool g_chordTextBuilt = false;

/// The row being rebound in Preferences; Count while none.
EditorAction g_capturing = EditorAction::Count;

/// Bindings read from the preferences section, applied together by
/// commit_stored_shortcuts.
std::array<ImGuiKeyChord, kShortcuts.size()> g_staged =
    std::array<ImGuiKeyChord, kShortcuts.size()>();
std::array<bool, kShortcuts.size()> g_stagedSet =
    std::array<bool, kShortcuts.size()>();

/// A key's saved and shown name. The engine names keys itself: ImGui's
/// key names are for debugging and may change between versions, and a
/// saved binding must read back the same.
struct KeyName final {
  ImGuiKey key = ImGuiKey_None;
  const char *name = nullptr;
};

constexpr KeyName kNamedKeys[] = {
    {ImGuiKey_Delete, "Delete"},
    {ImGuiKey_Backspace, "Backspace"},
    {ImGuiKey_Insert, "Insert"},
    {ImGuiKey_Home, "Home"},
    {ImGuiKey_End, "End"},
    {ImGuiKey_PageUp, "PageUp"},
    {ImGuiKey_PageDown, "PageDown"},
    {ImGuiKey_Space, "Space"},
    {ImGuiKey_Enter, "Enter"},
    {ImGuiKey_Escape, "Escape"},
    {ImGuiKey_Tab, "Tab"},
    {ImGuiKey_LeftArrow, "Left"},
    {ImGuiKey_RightArrow, "Right"},
    {ImGuiKey_UpArrow, "Up"},
    {ImGuiKey_DownArrow, "Down"},
    {ImGuiKey_Minus, "Minus"},
    {ImGuiKey_Equal, "Equal"},
    {ImGuiKey_LeftBracket, "LeftBracket"},
    {ImGuiKey_RightBracket, "RightBracket"},
    {ImGuiKey_Semicolon, "Semicolon"},
    {ImGuiKey_Apostrophe, "Apostrophe"},
    {ImGuiKey_Comma, "Comma"},
    {ImGuiKey_Period, "Period"},
    {ImGuiKey_Slash, "Slash"},
    {ImGuiKey_Backslash, "Backslash"},
    {ImGuiKey_GraveAccent, "GraveAccent"},
};

/// Writes `key`'s name into `out`; false for a key a binding cannot use.
bool key_name(ImGuiKey key, char *out, std::size_t capacity) noexcept {
  if ((key >= ImGuiKey_A) && (key <= ImGuiKey_Z)) {
    std::snprintf(out, capacity, "%c", 'A' + (key - ImGuiKey_A));
    return true;
  }
  if ((key >= ImGuiKey_0) && (key <= ImGuiKey_9)) {
    std::snprintf(out, capacity, "%c", '0' + (key - ImGuiKey_0));
    return true;
  }
  if ((key >= ImGuiKey_F1) && (key <= ImGuiKey_F12)) {
    std::snprintf(out, capacity, "F%d", 1 + (key - ImGuiKey_F1));
    return true;
  }
  for (const KeyName &named : kNamedKeys) {
    if (named.key == key) {
      std::snprintf(out, capacity, "%s", named.name);
      return true;
    }
  }
  return false;
}

/// The key named `name` (exactly as key_name writes it), or ImGuiKey_None.
ImGuiKey key_from_name(const char *name, std::size_t length) noexcept {
  char buffer[16] = {};
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
    if (key_name(static_cast<ImGuiKey>(k), buffer, sizeof(buffer)) &&
        (std::strlen(buffer) == length) &&
        (std::strncmp(buffer, name, length) == 0)) {
      return static_cast<ImGuiKey>(k);
    }
  }
  return ImGuiKey_None;
}

/// Writes `chord` as menu text: platform modifier names (Cmd and Option on
/// a Mac) for display, or the saved names ("Ctrl+Shift+S") for a file.
void write_chord(ImGuiKeyChord chord, bool forDisplay, char *out,
                 std::size_t capacity) noexcept {
  out[0] = '\0';
  char key[16] = {};
  if ((chord == 0) || !key_name(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_),
                                key, sizeof(key))) {
    return;
  }
  const bool mac = forDisplay && ImGui::GetIO().ConfigMacOSXBehaviors;
  std::snprintf(
      out, capacity, "%s%s%s%s%s",
      ((chord & ImGuiMod_Ctrl) != 0) ? (mac ? "Cmd+" : "Ctrl+") : "",
      ((chord & ImGuiMod_Shift) != 0) ? "Shift+" : "",
      ((chord & ImGuiMod_Alt) != 0) ? (mac ? "Option+" : "Alt+") : "",
      ((chord & ImGuiMod_Super) != 0) ? (mac ? "Ctrl+" : "Super+") : "", key);
}

/// Makes `chord` row `index`'s primary chord, dropping an alternate it
/// now repeats.
void set_row_chord(std::size_t index, ImGuiKeyChord chord) noexcept {
  g_rows[index].chord = chord;
  if (g_rows[index].alternate == chord) {
    g_rows[index].alternate = 0;
  }
  g_chordTextBuilt = false;
}

/// True when rows `a` and `b` share a chord, primary or alternate.
bool rows_share_chord(const EditorShortcut &a,
                      const EditorShortcut &b) noexcept {
  const ImGuiKeyChord mine[] = {a.chord, a.alternate};
  for (const ImGuiKeyChord chord : mine) {
    if ((chord != 0) && ((chord == b.chord) || (chord == b.alternate))) {
      return true;
    }
  }
  return false;
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

std::size_t editor_shortcut_count() noexcept { return g_rows.size(); }

const EditorShortcut &editor_shortcut_at(std::size_t index) noexcept {
  return g_rows[(index < g_rows.size()) ? index : 0U];
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
  case EditorAction::CreateEmpty:
    return world_is_editable();
  case EditorAction::Exit:
    return true;
  case EditorAction::Undo:
    return editor_history_can_undo();
  case EditorAction::Redo:
    return editor_history_can_redo();
  case EditorAction::Copy:
    // Copy only reads, so it works during play; the copy pastes after
    // Stop, as in Unity.
    return (editor_session().world != nullptr) && has_selection();
  case EditorAction::Paste:
    return world_is_editable() && entity_clipboard_has();
  case EditorAction::PasteAsChild:
    return world_is_editable() && entity_clipboard_has() && has_selection();
  case EditorAction::Duplicate:
  case EditorAction::Delete:
    return world_is_editable() && has_selection();
  case EditorAction::GizmoTranslate:
  case EditorAction::GizmoRotate:
  case EditorAction::GizmoScale:
  case EditorAction::GizmoSpace:
    return true;
  case EditorAction::FrameSelected:
    return has_selection();
  case EditorAction::PlayStop:
    // Play needs a world that can enter play; Stop needs a session.
    return (editor_session().world != nullptr) &&
           ((editor_session().playState != PlayState::Stopped) ||
            !editor_session().worldRestoreFailed);
  case EditorAction::Pause:
  case EditorAction::Step:
    return (editor_session().world != nullptr) &&
           (editor_session().playState != PlayState::Stopped);
  case EditorAction::Screenshot:
    // A hidden Game view tab comes to the front to be taken.
    return true;
  case EditorAction::RecordPlay:
  case EditorAction::ReplayLatest:
    return recorded_play_can_start();
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
  case EditorAction::Exit:
    // The same guard a window close runs: play stops first, and a dirty
    // document asks before anything is lost.
    if (editor_handle_quit_request()) {
      core::request_platform_quit();
    }
    return true;
  case EditorAction::Undo:
    editor_history_undo();
    return true;
  case EditorAction::Redo:
    editor_history_redo();
    return true;
  case EditorAction::Copy:
    return entity_clipboard_copy();
  case EditorAction::Paste:
    return execute_entity_paste(runtime::kInvalidEntity);
  case EditorAction::PasteAsChild:
    return execute_entity_paste(selected_entity());
  case EditorAction::Duplicate:
    return execute_selection_duplicate();
  case EditorAction::Delete:
    return execute_selection_delete();
  case EditorAction::CreateEmpty: {
    const runtime::Entity created = execute_entity_create();
    if (created == runtime::kInvalidEntity) {
      return false;
    }
    select_entity(created, false);
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
  case EditorAction::FrameSelected:
    return frame_selection();
  case EditorAction::GizmoSpace:
    editor_session().gizmoWorldSpace = !editor_session().gizmoWorldSpace;
    if (ImGui::GetCurrentContext() != nullptr) {
      ImGui::MarkIniSettingsDirty(); // the choice is a saved preference
    }
    return true;
  case EditorAction::PlayStop:
    if (editor_session().playState == PlayState::Stopped) {
      start_play_mode();
    } else {
      stop_play_mode();
    }
    return true;
  case EditorAction::Pause:
    pause_play_mode();
    return true;
  case EditorAction::Step:
    // As in Unity, Step while playing pauses first, then steps once.
    if (editor_session().playState == PlayState::Playing) {
      pause_play_mode();
    }
    editor_session().stepRequested = true;
    return true;
  case EditorAction::Screenshot:
    request_game_view_screenshot();
    return true;
  case EditorAction::RecordPlay:
    return start_recorded_play(nullptr);
  case EditorAction::ReplayLatest:
    return start_replay(nullptr);
  case EditorAction::Count:
  default:
    return false;
  }
}

const char *editor_shortcut_text(EditorAction action) noexcept {
  if (!g_chordTextBuilt) {
    for (std::size_t i = 0U; i < g_rows.size(); ++i) {
      write_chord(g_rows[i].chord, true, g_chordText[i].data(),
                  g_chordText[i].size());
    }
    g_chordTextBuilt = true;
  }
  const auto index = static_cast<std::size_t>(action);
  return (index < g_chordText.size()) ? g_chordText[index].data() : "";
}

bool format_key_chord(ImGuiKeyChord chord, char *out,
                      std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  if (chord == 0) {
    std::snprintf(out, capacity, "None");
    return true;
  }
  write_chord(chord, false, out, capacity);
  return out[0] != '\0';
}

bool parse_key_chord(const char *text, ImGuiKeyChord *out) noexcept {
  if ((text == nullptr) || (out == nullptr)) {
    return false;
  }
  if (std::strcmp(text, "None") == 0) {
    *out = 0;
    return true;
  }
  ImGuiKeyChord mods = 0;
  const char *cursor = text;
  constexpr struct {
    const char *prefix;
    ImGuiKeyChord mod;
  } kMods[] = {{"Ctrl+", ImGuiMod_Ctrl},
               {"Shift+", ImGuiMod_Shift},
               {"Alt+", ImGuiMod_Alt},
               {"Super+", ImGuiMod_Super}};
  // Modifiers come first, each once, in the order write_chord writes them.
  for (const auto &mod : kMods) {
    const std::size_t length = std::strlen(mod.prefix);
    if (std::strncmp(cursor, mod.prefix, length) == 0) {
      mods |= mod.mod;
      cursor += length;
    }
  }
  const ImGuiKey key = key_from_name(cursor, std::strlen(cursor));
  if (key == ImGuiKey_None) {
    return false;
  }
  *out = mods | key;
  return true;
}

bool find_editor_action(const char *id, EditorAction *out) noexcept {
  for (const EditorShortcut &row : g_rows) {
    if ((id != nullptr) && (std::strcmp(row.id, id) == 0)) {
      if (out != nullptr) {
        *out = row.action;
      }
      return true;
    }
  }
  return false;
}

bool rebind_editor_action(EditorAction action, ImGuiKeyChord chord,
                          EditorAction *outConflict) noexcept {
  const auto index = static_cast<std::size_t>(action);
  if (index >= g_rows.size()) {
    return false;
  }
  char probe[40] = {};
  if ((chord != 0) && !format_key_chord(chord, probe, sizeof(probe))) {
    return false; // a modifier alone, or a key a binding cannot name
  }
  for (const EditorShortcut &row : g_rows) {
    if ((chord != 0) && (row.action != action) &&
        ((row.chord == chord) || (row.alternate == chord))) {
      if (outConflict != nullptr) {
        *outConflict = row.action;
      }
      return false;
    }
  }
  set_row_chord(index, chord);
  return true;
}

bool editor_action_rebound(EditorAction action) noexcept {
  const auto index = static_cast<std::size_t>(action);
  return (index < g_rows.size()) &&
         ((g_rows[index].chord != kShortcuts[index].chord) ||
          (g_rows[index].alternate != kShortcuts[index].alternate));
}

void reset_editor_shortcuts() noexcept {
  g_rows = kShortcuts;
  g_chordTextBuilt = false;
  g_capturing = EditorAction::Count;
}

void begin_stored_shortcuts() noexcept { g_stagedSet.fill(false); }

bool stage_stored_shortcut(const char *id, const char *chordText) noexcept {
  EditorAction action = EditorAction::Count;
  ImGuiKeyChord chord = 0;
  char message[160] = {};
  if (!find_editor_action(id, &action)) {
    std::snprintf(message, sizeof(message),
                  "stored shortcut for unknown action '%s' ignored", id);
  } else if (!parse_key_chord(chordText, &chord)) {
    std::snprintf(message, sizeof(message),
                  "stored shortcut '%s' for %s is not a key chord; the "
                  "default stays",
                  chordText, id);
  } else {
    const auto index = static_cast<std::size_t>(action);
    g_staged[index] = chord;
    g_stagedSet[index] = true;
    return true;
  }
  core::log_message(core::LogLevel::Warning, "editor", message);
  return false;
}

void commit_stored_shortcuts() noexcept {
  g_rows = kShortcuts;
  g_chordTextBuilt = false;
  g_capturing = EditorAction::Count;
  for (std::size_t i = 0U; i < g_rows.size(); ++i) {
    if (g_stagedSet[i]) {
      set_row_chord(i, g_staged[i]);
    }
  }
  // Conflicts are judged on the final table, so one line may take a chord
  // a later line frees. A shared chord reverts a rebound row to its
  // default (the later one when both are rebound; two defaults never
  // share), and the table is checked again: each pass reverts a rebound
  // row, so the loop ends.
  for (bool shared = true; shared;) {
    shared = false;
    for (std::size_t a = 0U; !shared && (a < g_rows.size()); ++a) {
      for (std::size_t b = a + 1U; !shared && (b < g_rows.size()); ++b) {
        if (!rows_share_chord(g_rows[a], g_rows[b])) {
          continue;
        }
        shared = true;
        const std::size_t revert =
            editor_action_rebound(g_rows[b].action) ? b : a;
        const std::size_t holder = (revert == b) ? a : b;
        char message[160] = {};
        std::snprintf(message, sizeof(message),
                      "stored shortcut for %s is already %s's; the default "
                      "stays",
                      g_rows[revert].id, g_rows[holder].id);
        core::log_message(core::LogLevel::Warning, "editor", message);
        g_rows[revert] = kShortcuts[revert];
      }
    }
  }
}

void begin_shortcut_capture(EditorAction action) noexcept {
  g_capturing = action;
}

EditorAction shortcut_capture_target() noexcept { return g_capturing; }

void end_shortcut_capture() noexcept { g_capturing = EditorAction::Count; }

bool editor_shortcuts_blocked() noexcept {
  const ImGuiIO &io = ImGui::GetIO();
  // A popup (a menu, a context menu, a combo, a modal) takes the keyboard
  // while open, and the unsaved-changes prompt counts from the moment it
  // is armed, before its modal is drawn.
  // A chord being captured for a rebinding is not a command either, and
  // while the Scene camera flies WASD/QE move it.
  return io.WantTextInput || scene_document_prompt_open() ||
         (g_capturing != EditorAction::Count) || editor_session().sceneFlying ||
         ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel);
}

void update_focused_panel() noexcept {
  const ImGuiContext *context = ImGui::GetCurrentContext();
  if ((context == nullptr) || (context->NavWindow == nullptr)) {
    return;
  }
  // A child of a panel (the Scene view's image, a list) counts as the
  // panel; a popup or menu is a root of its own and matches none.
  const ImGuiWindow *root = context->NavWindow->RootWindow;
  const char *name = (root != nullptr) ? root->Name : "";
  struct PanelName final {
    const char *name;
    EditorPanel panel;
  };
  static constexpr std::array<PanelName, 6U> kPanels = {{
      {kSceneViewWindow, EditorPanel::Scene},
      {kGameViewWindow, EditorPanel::Game},
      {kEntitiesWindow, EditorPanel::Entities},
      {kInspectorWindow, EditorPanel::Inspector},
      {kAssetsWindow, EditorPanel::Assets},
      {kLogWindow, EditorPanel::Log},
  }};
  for (const PanelName &entry : kPanels) {
    if (std::strcmp(name, entry.name) == 0) {
      editor_session().lastFocusedPanel = entry.panel;
      return;
    }
  }
}

bool editor_action_in_focus_scope(EditorAction action) noexcept {
  if (!editor_shortcut(action).sceneEditing) {
    return true;
  }
  const EditorPanel panel = editor_session().lastFocusedPanel;
  return (panel == EditorPanel::Scene) || (panel == EditorPanel::Entities);
}

void dispatch_editor_shortcuts() noexcept {
  update_focused_panel();
  if (editor_shortcuts_blocked()) {
    return;
  }
  // While the game has the keyboard its keys are the game's (W is a move,
  // not a gizmo), except the play controls.
  const bool gameHasKeyboard = game_owns_keyboard();
  for (const EditorShortcut &row : g_rows) {
    if ((gameHasKeyboard && !row.whileGameHasKeyboard) ||
        !editor_action_in_focus_scope(row.action)) {
      continue;
    }
    if (chord_pressed(row.chord, row.repeats) ||
        chord_pressed(row.alternate, row.repeats)) {
      static_cast<void>(run_editor_action(row.action));
    }
  }
}

const char *editor_action_label(EditorAction action) noexcept {
  if ((action == EditorAction::PlayStop) &&
      (editor_session().playState != PlayState::Stopped)) {
    return "Stop";
  }
  return editor_shortcut(action).label;
}

bool editor_action_menu_item_clicked(EditorAction action,
                                     bool checked) noexcept {
  return ImGui::MenuItem(editor_action_label(action),
                         editor_shortcut_text(action), checked,
                         editor_action_enabled(action));
}

bool editor_edit_menu_item(EditorAction action, bool checked) noexcept {
  const bool enabled =
      editor_action_in_focus_scope(action) && editor_action_enabled(action);
  return ImGui::MenuItem(editor_action_label(action),
                         editor_shortcut_text(action), checked, enabled) &&
         run_editor_action(action);
}

bool editor_action_menu_item(EditorAction action, bool checked) noexcept {
  return editor_action_menu_item_clicked(action, checked) &&
         run_editor_action(action);
}

} // namespace engine::editor
