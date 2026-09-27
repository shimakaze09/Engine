// Verifies the editor's action table and its key dispatch through the
// production dispatcher on a headless ImGui frame. It covers chords
// matched exactly, modifiers included (Ctrl+Shift+S is Save As, not Save,
// and Ctrl+R is not the scale tool), creating actions that run once per
// press while undo repeats, dispatch stopping under the unsaved-changes
// prompt, a popup, a text field and a game-owned keyboard, the play
// chords that alone stay live while the game has the keyboard, rebinding
// with its persistence and refusals, and table invariants: one row per
// action, unique ids, no chord bound twice.

#include "editor_commands.h"
#include "editor_preferences.h"
#include "editor_scene_document.h"
#include "editor_scene_document_fixture.h"
#include "editor_session.h"
#include "editor_shortcuts.h"

#include "imgui_internal.h"

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/editor/editor.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

using namespace engine::editor;
using namespace engine::runtime;

constexpr const char *kScratchRoot = "assets/engine_editor_shortcuts_test";

/// One headless frame: key events queued before it are applied by
/// NewFrame, then the production dispatcher runs, as editor_new_frame
/// does. `drawPopup` keeps a popup open for the frames that want one;
/// `focusText` gives a text field the keyboard.
struct FrameOptions final {
  bool openPopup = false;
  bool keepPopup = false;
  bool focusText = false;
};

void run_frame(const FrameOptions &options = FrameOptions{}) noexcept {
  ImGui::NewFrame();
  dispatch_editor_shortcuts();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(300.0F, 200.0F));
  ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoSavedSettings);
  if (options.openPopup) {
    ImGui::OpenPopup("Probe");
  }
  if ((options.openPopup || options.keepPopup) && ImGui::BeginPopup("Probe")) {
    ImGui::TextUnformatted("probe");
    ImGui::EndPopup();
  }
  static char text[32] = {};
  if (options.focusText) {
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::InputText("Name", text, sizeof(text));
  ImGui::End();
  ImGui::Render();
}

void set_keys(ImGuiKeyChord chord, bool down) noexcept {
  ImGuiIO &io = ImGui::GetIO();
  if ((chord & ImGuiMod_Ctrl) != 0) {
    io.AddKeyEvent(ImGuiMod_Ctrl, down);
  }
  if ((chord & ImGuiMod_Shift) != 0) {
    io.AddKeyEvent(ImGuiMod_Shift, down);
  }
  if ((chord & ImGuiMod_Alt) != 0) {
    io.AddKeyEvent(ImGuiMod_Alt, down);
  }
  io.AddKeyEvent(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_), down);
}

/// Presses and releases `chord` over two frames.
void tap(ImGuiKeyChord chord, const FrameOptions &options = FrameOptions{}) {
  set_keys(chord, true);
  run_frame(options);
  set_keys(chord, false);
  run_frame(options);
}

/// Holds `chord` for `frames` frames of the test's frame time, then
/// releases it.
void hold(ImGuiKeyChord chord, int frames) noexcept {
  set_keys(chord, true);
  for (int i = 0; i < frames; ++i) {
    run_frame();
  }
  set_keys(chord, false);
  run_frame();
}

bool scratch_path(const char *leaf, char *out, std::size_t capacity) noexcept {
  std::error_code ec{};
  std::filesystem::create_directories(kScratchRoot, ec);
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(
      std::filesystem::path(kScratchRoot) / leaf, ec);
  if (ec) {
    return false;
  }
  const std::string text = resolved.string();
  const int written = std::snprintf(out, capacity, "%s", text.c_str());
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

Entity add_named(World &world, const char *name) noexcept {
  const Entity entity = world.create_scene_object();
  if (entity == kInvalidEntity) {
    return kInvalidEntity;
  }
  NameComponent component{};
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  return world.add_name_component(entity, component) ? entity : kInvalidEntity;
}

bool file_holds(const char *path, const char *name) noexcept {
  std::unique_ptr<World> reader(new (std::nothrow) World());
  return (reader != nullptr) && load_scene(*reader, path) &&
         (reader->find_entity_by_name(name) != kInvalidEntity);
}

/// Cancels whichever native dialog the session is waiting on.
void cancel_pending_dialog() noexcept {
  const engine::core::FileDialogTicket ticket =
      editor_session().document.activeDialog;
  if (ticket != engine::core::kNoFileDialog) {
    static_cast<void>(
        engine::core::platform_answer_scripted_file_dialog(ticket, nullptr));
    scene_document_poll_dialog_result();
  }
}

void check_table_invariants(engine::tests::TestContext &t) noexcept {
  const std::size_t count = editor_shortcut_count();
  t.check(count == static_cast<std::size_t>(EditorAction::Count),
          "one row per action");
  bool rowsMatch = true;
  bool idsUnique = true;
  bool chordsUnique = true;
  for (std::size_t i = 0U; i < count; ++i) {
    const EditorShortcut &row = editor_shortcut_at(i);
    rowsMatch = rowsMatch && (static_cast<std::size_t>(row.action) == i) &&
                (row.id != nullptr) && (row.label != nullptr);
    for (std::size_t j = i + 1U; j < count; ++j) {
      const EditorShortcut &other = editor_shortcut_at(j);
      idsUnique = idsUnique && (std::strcmp(row.id, other.id) != 0);
      const ImGuiKeyChord mine[] = {row.chord, row.alternate};
      const ImGuiKeyChord theirs[] = {other.chord, other.alternate};
      for (const ImGuiKeyChord a : mine) {
        for (const ImGuiKeyChord b : theirs) {
          chordsUnique = chordsUnique && ((a == 0) || (a != b));
        }
      }
    }
    chordsUnique =
        chordsUnique && ((row.alternate == 0) || (row.alternate != row.chord));
  }
  t.check(rowsMatch, "each row is its action's, with an id and a label");
  t.check(idsUnique, "row ids are unique");
  t.check(chordsUnique, "no chord runs two actions");
  t.check(std::strcmp(editor_shortcut_text(EditorAction::SaveSceneAs),
                      "Ctrl+Shift+S") == 0,
          "the menu shows Save As's chord from the table");
  t.check(std::strcmp(editor_shortcut_text(EditorAction::GizmoScale), "R") == 0,
          "a bare key shows as the key alone");
}

void check_save_chords(engine::tests::TestContext &t, World &world) noexcept {
  char path[512] = {};
  if (!scratch_path("save_chords.json", path, sizeof(path))) {
    t.fail("scratch path");
    return;
  }
  static_cast<void>(std::remove(path));
  t.check(perform_scene_new() &&
              (add_named(world, "First") != kInvalidEntity) &&
              perform_scene_save_as(path),
          "the scene has a path");
  t.check(add_named(world, "Second") != kInvalidEntity, "a second entity");

  // Ctrl+Shift+S is Save As: a dialog, and the file is not overwritten.
  // On base the Ctrl+S test matched it and saved in place.
  tap(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S);
  t.check(editor_session().document.activeDialog != engine::core::kNoFileDialog,
          "Ctrl+Shift+S opens the Save As dialog");
  t.check(!file_holds(path, "Second"),
          "Ctrl+Shift+S does not save over the open scene");
  cancel_pending_dialog();

  // Ctrl+S saves in place.
  tap(ImGuiMod_Ctrl | ImGuiKey_S);
  t.check(file_holds(path, "Second"), "Ctrl+S saves in place");
  t.check(editor_session().document.activeDialog == engine::core::kNoFileDialog,
          "Ctrl+S opens no dialog for a titled scene");
}

void check_tool_keys_match_exactly(engine::tests::TestContext &t) noexcept {
  editor_session().gizmoOp = ImGuizmo::TRANSLATE;
  tap(ImGuiMod_Ctrl | ImGuiKey_R);
  t.check(editor_session().gizmoOp == ImGuizmo::TRANSLATE,
          "Ctrl+R does not pick the scale tool");
  tap(ImGuiMod_Alt | ImGuiKey_E);
  t.check(editor_session().gizmoOp == ImGuizmo::TRANSLATE,
          "Alt+E does not pick the rotate tool");
  tap(ImGuiKey_R);
  t.check(editor_session().gizmoOp == ImGuizmo::SCALE, "R picks scale");
  tap(ImGuiKey_E);
  t.check(editor_session().gizmoOp == ImGuizmo::ROTATE, "E picks rotate");
  tap(ImGuiKey_W);
  t.check(editor_session().gizmoOp == ImGuizmo::TRANSLATE, "W picks move");
}

void check_repeat_policy(engine::tests::TestContext &t, World &world) noexcept {
  t.check(perform_scene_new(), "fresh scene");
  const Entity original = add_named(world, "Original");
  select_entity(original, false);
  const std::size_t before = world.alive_entity_count();

  // Held for a second, well past the key-repeat delay: one copy. On base
  // Ctrl+D repeated and made a copy per repeat.
  hold(ImGuiMod_Ctrl | ImGuiKey_D, 20);
  t.check(world.alive_entity_count() == before + 1U,
          "holding Ctrl+D duplicates once");

  // Undo repeats while held, as in other editors.
  select_entity(original, false);
  t.check(run_editor_action(EditorAction::Duplicate), "second copy");
  select_entity(original, false);
  t.check(run_editor_action(EditorAction::Duplicate), "third copy");
  t.check(world.alive_entity_count() == before + 3U, "three copies");
  hold(ImGuiMod_Ctrl | ImGuiKey_Z, 20);
  t.check(world.alive_entity_count() == before,
          "holding Ctrl+Z undoes every copy");

  // Ctrl+Y is Redo's second chord.
  tap(ImGuiMod_Ctrl | ImGuiKey_Y);
  t.check(world.alive_entity_count() == before + 1U, "Ctrl+Y redoes");
  tap(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
  t.check(world.alive_entity_count() == before + 2U, "Ctrl+Shift+Z redoes");
}

void check_blocked_contexts(engine::tests::TestContext &t,
                            World &world) noexcept {
  t.check(perform_scene_new(), "fresh scene");
  const Entity original = add_named(world, "Original");
  select_entity(original, false);
  t.check(run_editor_action(EditorAction::Duplicate), "a dirty document");
  select_entity(original, false);

  // The unsaved-changes prompt is modal: nothing behind it changes. On
  // base Ctrl+D duplicated under it.
  request_scene_new();
  t.check(scene_document_prompt_open(), "New asks first");
  std::size_t count = world.alive_entity_count();
  tap(ImGuiMod_Ctrl | ImGuiKey_D);
  t.check(world.alive_entity_count() == count,
          "no duplicate under the unsaved-changes prompt");
  scene_document_prompt_choose_cancel();

  // An open popup (a menu, a context menu) takes the keyboard.
  FrameOptions popup{};
  popup.openPopup = true;
  run_frame(popup);
  popup.openPopup = false;
  popup.keepPopup = true;
  editor_session().gizmoOp = ImGuizmo::TRANSLATE;
  tap(ImGuiKey_R, popup);
  t.check(editor_session().gizmoOp == ImGuizmo::TRANSLATE,
          "no tool change while a popup is open");
  run_frame(); // the popup is not drawn, so it closes

  // A text field with the keyboard types the letter instead.
  // The focus request lands a frame later, and io.WantTextInput reports
  // the field a frame after that.
  FrameOptions focus{};
  focus.focusText = true;
  run_frame(focus);
  run_frame();
  run_frame();
  t.check(ImGui::GetIO().WantTextInput, "the text field has the keyboard");
  tap(ImGuiKey_R);
  t.check(editor_session().gizmoOp == ImGuizmo::TRANSLATE,
          "no tool change while typing");
  ImGui::ClearActiveID();
  run_frame();
  run_frame();

  // The game has the keyboard while its view is focused in play.
  editor_session().playState = PlayState::Playing;
  editor_session().gameViewFocused = true;
  tap(ImGuiKey_R);
  t.check(editor_session().gizmoOp == ImGuizmo::TRANSLATE,
          "no tool change while the game has the keyboard");
  editor_session().gameViewFocused = false;
  editor_session().playState = PlayState::Stopped;

  count = world.alive_entity_count();
  tap(ImGuiMod_Ctrl | ImGuiKey_D);
  t.check(world.alive_entity_count() == count + 1U,
          "dispatch resumes once nothing holds the keyboard");
}

void check_document_chords(engine::tests::TestContext &t,
                           World &world) noexcept {
  t.check(perform_scene_new() && (add_named(world, "Loose") != kInvalidEntity),
          "an unsaved entity in a clean document");
  tap(ImGuiMod_Ctrl | ImGuiKey_N);
  t.check(world.alive_entity_count() == 0U, "Ctrl+N starts a new scene");
  tap(ImGuiMod_Ctrl | ImGuiKey_O);
  t.check(editor_session().document.activeDialog != engine::core::kNoFileDialog,
          "Ctrl+O opens the Open dialog");
  cancel_pending_dialog();
}

/// Unity's play chords: Ctrl+P plays and stops, Ctrl+Shift+P pauses and
/// resumes, Ctrl+Alt+P steps (pausing first when playing). They are the
/// only chords that still fire while the game has the keyboard.
void check_play_chords(engine::tests::TestContext &t, World &world) noexcept {
  t.check(perform_scene_new() && (add_named(world, "Actor") != kInvalidEntity),
          "a scene to play");
  tap(ImGuiMod_Ctrl | ImGuiKey_P);
  t.check(editor_session().playState == PlayState::Playing, "Ctrl+P plays");

  // The Game view has the keyboard now: tool keys are the game's, but the
  // play controls still reach the editor.
  editor_session().gameViewFocused = true;
  editor_session().gizmoOp = ImGuizmo::TRANSLATE;
  tap(ImGuiKey_R);
  t.check(editor_session().gizmoOp == ImGuizmo::TRANSLATE,
          "R is the game's while it has the keyboard");
  tap(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P);
  t.check(editor_session().playState == PlayState::Paused,
          "Ctrl+Shift+P pauses while the game has the keyboard");
  tap(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P);
  t.check(editor_session().playState == PlayState::Playing,
          "Ctrl+Shift+P again resumes");
  editor_session().stepRequested = false;
  tap(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_P);
  t.check((editor_session().playState == PlayState::Paused) &&
              editor_session().stepRequested,
          "Ctrl+Alt+P pauses and steps once");
  tap(ImGuiMod_Ctrl | ImGuiKey_P);
  finish_play_stop();
  t.check(editor_session().playState == PlayState::Stopped,
          "Ctrl+P again stops");
  editor_session().gameViewFocused = false;
  editor_session().stepRequested = false;
}

/// Create Empty (Ctrl+Shift+N) makes an entity and selects it; Exit runs
/// the window close's quit guard, so a dirty document asks before the
/// editor quits.
void check_create_and_exit(engine::tests::TestContext &t,
                           World &world) noexcept {
  t.check(perform_scene_new(), "a fresh scene");
  tap(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N);
  t.check((world.alive_entity_count() == 1U) &&
              (selected_entity() != kInvalidEntity),
          "Ctrl+Shift+N creates an entity and selects it");
  t.check(scene_document_is_dirty(), "the new entity dirties the document");

  // The quit guard only protects a session the editor initialized.
  editor_session().initialized = true;
  t.check(run_editor_action(EditorAction::Exit), "File > Exit runs");
  editor_session().initialized = false;
  t.check(scene_document_prompt_open(),
          "a dirty document asks before the editor quits");
  scene_document_prompt_choose_cancel();
}

/// Reads `lines` as the stored preferences section, as the layout file
/// load does.
void load_section(const char *lines) noexcept {
  char text[512] = {};
  std::snprintf(text, sizeof(text), "[EnginePreferences][Editor]\n%s\n", lines);
  ImGui::LoadIniSettingsFromMemory(text, std::strlen(text));
}

/// A rebinding moves the action to its new chord; one that collides is
/// refused and names the holder. Rebound chords travel through the
/// preferences section as Shortcut.<id>=<chord> lines. Reading them back
/// refuses an unknown id, a malformed chord and a chord two actions would
/// share, judged on the final table, while applying the rest. Every
/// default chord survives a format-then-parse round trip.
void check_rebinding(engine::tests::TestContext &t, World &world) noexcept {
  bool roundTrips = true;
  for (std::size_t i = 0U; i < editor_shortcut_count(); ++i) {
    const EditorShortcut &row = editor_shortcut_at(i);
    char text[40] = {};
    ImGuiKeyChord parsed = -1;
    roundTrips = roundTrips &&
                 format_key_chord(row.chord, text, sizeof(text)) &&
                 parse_key_chord(text, &parsed) && (parsed == row.chord);
  }
  t.check(roundTrips, "every default chord saves and reads back");

  const ImGuiKeyChord moved = ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_D;
  t.check(rebind_editor_action(EditorAction::Duplicate, moved, nullptr),
          "rebind Duplicate");
  t.check(perform_scene_new(), "fresh scene");
  const Entity entity = add_named(world, "Rebound");
  select_entity(entity, false);
  std::size_t count = world.alive_entity_count();
  tap(ImGuiMod_Ctrl | ImGuiKey_D);
  t.check(world.alive_entity_count() == count,
          "the old chord no longer duplicates");
  tap(moved);
  t.check(world.alive_entity_count() == count + 1U, "the new chord duplicates");
  t.check(std::strcmp(editor_shortcut_text(EditorAction::Duplicate),
                      "Ctrl+Alt+D") == 0,
          "the menu shows the new chord");

  EditorAction conflict = EditorAction::Count;
  t.check(!rebind_editor_action(EditorAction::Duplicate,
                                ImGuiMod_Ctrl | ImGuiKey_S, &conflict) &&
              (conflict == EditorAction::SaveScene),
          "a chord Save holds is refused, naming Save");

  char section[2048] = {};
  t.check((engine::editor::editor_preferences_section(section,
                                                      sizeof(section)) > 0U) &&
              (std::strstr(section, "Shortcut.edit.duplicate=Ctrl+Alt+D\n") !=
               nullptr) &&
              (std::strstr(section, "Shortcut.edit.undo") == nullptr),
          "only the rebound action is saved, with its saved names");

  // The saved section reads back to the same table.
  reset_editor_shortcuts();
  ImGui::LoadIniSettingsFromMemory(section, std::strlen(section));
  bool sameTable = true;
  for (std::size_t i = 0U; i < editor_shortcut_count(); ++i) {
    const EditorShortcut &row = editor_shortcut_at(i);
    sameTable = sameTable && (row.action == EditorAction::Duplicate
                                  ? (row.chord == moved)
                                  : !editor_action_rebound(row.action));
  }
  t.check(sameTable, "the saved section reads back to the same bindings");

  load_section("Shortcut.edit.duplicate=Ctrl+Alt+D\n"
               "Shortcut.no.such_action=Ctrl+K\n"
               "Shortcut.edit.copy=Ctrl+Nope\n"
               "Shortcut.edit.paste=Ctrl+S\n");
  t.check(editor_shortcut(EditorAction::Duplicate).chord == moved,
          "a stored rebinding is applied");
  t.check(editor_shortcut(EditorAction::Copy).chord ==
              (ImGuiMod_Ctrl | ImGuiKey_C),
          "a malformed stored chord leaves the default");
  t.check(editor_shortcut(EditorAction::Paste).chord ==
              (ImGuiMod_Ctrl | ImGuiKey_V),
          "a stored chord another action holds leaves the default");

  // Copy is saved before Delete, so its line takes the chord Delete's
  // later line frees: conflicts are judged on the final table.
  load_section("Shortcut.edit.copy=Delete\n"
               "Shortcut.edit.delete=Ctrl+K\n");
  t.check((editor_shortcut(EditorAction::Copy).chord == ImGuiKey_Delete) &&
              (editor_shortcut(EditorAction::Delete).chord ==
               (ImGuiMod_Ctrl | ImGuiKey_K)),
          "a stored line may take a chord a later line frees");
  t.check(!editor_action_rebound(EditorAction::Duplicate),
          "a load replaces the bindings it does not name with defaults");

  // Delete's line is refused, so Delete keeps its key, and Copy's claim
  // on it is refused in turn.
  load_section("Shortcut.edit.copy=Delete\n"
               "Shortcut.edit.delete=Ctrl+Nope\n");
  t.check((editor_shortcut(EditorAction::Delete).chord == ImGuiKey_Delete) &&
              (editor_shortcut(EditorAction::Copy).chord ==
               (ImGuiMod_Ctrl | ImGuiKey_C)),
          "a refusal that keeps a default refuses the claim on it");

  load_section("Shortcut.edit.copy=Ctrl+J\n"
               "Shortcut.edit.paste=Ctrl+J\n");
  t.check((editor_shortcut(EditorAction::Copy).chord ==
           (ImGuiMod_Ctrl | ImGuiKey_J)) &&
              (editor_shortcut(EditorAction::Paste).chord ==
               (ImGuiMod_Ctrl | ImGuiKey_V)),
          "of two stored claims on one chord, the later row's is refused");

  // Remove Shortcut unbinds, and the removal is saved.
  reset_editor_shortcuts();
  t.check(rebind_editor_action(EditorAction::Duplicate, 0, nullptr),
          "remove Duplicate's shortcut");
  t.check(
      (engine::editor::editor_preferences_section(section, sizeof(section)) >
       0U) &&
          (std::strstr(section, "Shortcut.edit.duplicate=None\n") != nullptr),
      "a removed shortcut is saved as None");
  reset_editor_shortcuts();
  ImGui::LoadIniSettingsFromMemory(section, std::strlen(section));
  t.check(editor_shortcut(EditorAction::Duplicate).chord == 0,
          "a removed shortcut stays removed");
  count = world.alive_entity_count();
  select_entity(entity, false);
  tap(ImGuiMod_Ctrl | ImGuiKey_D);
  t.check(world.alive_entity_count() == count,
          "a removed shortcut does nothing");

  reset_editor_shortcuts();
  t.check(!editor_action_rebound(EditorAction::Duplicate),
          "Restore defaults restores every chord");
}

/// X switches the move and rotate handles between the world's axes and
/// the entity's own, as Unity's handle-rotation toggle does; the choice is
/// saved as GizmoSpace=World or Local, and anything else stored is
/// refused with the current choice kept.
void check_gizmo_space(engine::tests::TestContext &t) noexcept {
  engine::editor::EditorSession &session = engine::editor::editor_session();
  session.gizmoWorldSpace = false;
  tap(ImGuiKey_X);
  t.check(session.gizmoWorldSpace, "X switches the handles to world axes");
  tap(ImGuiMod_Ctrl | ImGuiKey_X);
  t.check(session.gizmoWorldSpace, "Ctrl+X is not X");
  char section[2048] = {};
  t.check((engine::editor::editor_preferences_section(section,
                                                      sizeof(section)) > 0U) &&
              (std::strstr(section, "GizmoSpace=World\n") != nullptr),
          "world axes are saved");
  load_section("GizmoSpace=Local\n");
  t.check(!session.gizmoWorldSpace, "a stored Local is applied");
  tap(ImGuiKey_X);
  load_section("GizmoSpace=Sideways\n");
  t.check(session.gizmoWorldSpace, "a malformed GizmoSpace keeps the choice");
  tap(ImGuiKey_X);
  t.check(!session.gizmoWorldSpace, "X switches back to the entity's axes");
}

/// The Scene grid's toggle is saved as ShowGrid=1 or 0; anything else
/// stored is refused with the current choice kept.
void check_grid_preference(engine::tests::TestContext &t) noexcept {
  engine::editor::EditorSession &session = engine::editor::editor_session();
  session.showGrid = true;
  char section[2048] = {};
  t.check((engine::editor::editor_preferences_section(section,
                                                      sizeof(section)) > 0U) &&
              (std::strstr(section, "ShowGrid=1\n") != nullptr),
          "a shown grid is saved");
  load_section("ShowGrid=0\n");
  t.check(!session.showGrid, "a stored hidden grid is applied");
  load_section("ShowGrid=yes\n");
  t.check(!session.showGrid, "a malformed ShowGrid keeps the choice");
  session.showGrid = true;
}

/// While the Scene camera flies, WASD/QE move it: the dispatcher stands
/// down so W is not the Move tool. The fly speed is saved and read back
/// exactly; a stored speed out of range is clamped, and one that is not a
/// positive number is refused with the current speed kept.
void check_flying(engine::tests::TestContext &t) noexcept {
  engine::editor::EditorSession &session = engine::editor::editor_session();
  session.gizmoOp = ImGuizmo::ROTATE;
  session.sceneFlying = true;
  tap(ImGuiKey_W);
  t.check(session.gizmoOp == ImGuizmo::ROTATE, "W flies, not Move, in flight");
  session.sceneFlying = false;
  tap(ImGuiKey_W);
  t.check(session.gizmoOp == ImGuizmo::TRANSLATE, "W is Move again after");

  session.editorCamera.flySpeed = 5.0F * 1.2F * 1.2F * 1.2F;
  const float saved = session.editorCamera.flySpeed;
  char section[2048] = {};
  t.check(engine::editor::editor_preferences_section(section, sizeof(section)) >
              0U,
          "the section is written");
  session.editorCamera.flySpeed = 1.0F;
  ImGui::LoadIniSettingsFromMemory(section, std::strlen(section));
  t.check(session.editorCamera.flySpeed == saved,
          "the fly speed reads back exactly");
  load_section("CameraSpeed=500\n");
  t.check(session.editorCamera.flySpeed ==
              engine::editor::EditorCamera::kMaxFlySpeed,
          "a stored speed out of range is clamped");
  load_section("CameraSpeed=0\n");
  t.check(session.editorCamera.flySpeed ==
              engine::editor::EditorCamera::kMaxFlySpeed,
          "a zero speed is refused");
  load_section("CameraSpeed=fast\n");
  t.check(session.editorCamera.flySpeed ==
              engine::editor::EditorCamera::kMaxFlySpeed,
          "a malformed speed is refused");
  session.editorCamera.flySpeed = 5.0F;
}

} // namespace

int main() {
  engine::tests::TestContext t;
  static_cast<void>(engine::core::initialize_logging());

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 0.05F;
  io.IniFilename = nullptr;
  // One event per key per frame is what the checks reason about.
  io.ConfigInputTrickleEventQueue = false;
  io.ConfigMacOSXBehaviors = false;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  if (!engine::core::initialize_cvars()) {
    return 96;
  }
  engine::editor::register_editor_preferences();
  engine::tests::RecentScenesGuard recentGuard;
  std::error_code ec{};
  std::filesystem::create_directories(kScratchRoot, ec);
  if (ec || !recentGuard.arm("assets/engine_editor_shortcuts_test/recent")) {
    return 98;
  }
  engine::core::platform_set_scripted_file_dialogs(true);

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 97;
  }
  editor_set_world(world.get());

  check_table_invariants(t);
  check_save_chords(t, *world);
  check_tool_keys_match_exactly(t);
  check_repeat_policy(t, *world);
  check_blocked_contexts(t, *world);
  check_document_chords(t, *world);
  check_play_chords(t, *world);
  check_create_and_exit(t, *world);
  check_rebinding(t, *world);
  check_gizmo_space(t);
  check_grid_preference(t);
  check_flying(t);

  editor_set_world(nullptr);
  engine::core::platform_set_scripted_file_dialogs(false);
  t.check(recentGuard.disarm(), "the real recent-scenes file is untouched");
  ImGui::DestroyContext();
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  return t.finish("editor_shortcuts");
}
