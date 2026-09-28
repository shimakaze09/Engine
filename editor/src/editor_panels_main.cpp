// Implements the editor main menu bar, play toolbar, and entity hierarchy
// panel. Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_main.h"

#include "editor_commands.h"
#include "editor_hierarchy_walk.h"
#include "editor_material_edit.h"
#include "editor_panels_console.h"
#include "editor_panels_diagnostics.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "editor_shortcuts.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/engine_version.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/platform.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/editor/editor_camera.h"
#include "engine/engine.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/command_history.h"

#include <stb_image.h>

namespace engine::editor {

/// Draws the Save/Discard/Cancel confirm modal that gates
/// New/Open/quit while the document is dirty; the decision itself is
/// production logic in editor_scene_document.cpp/scene_document_prompt_*,
/// this function only presents it.
static void draw_unsaved_changes_prompt() noexcept {
  if (!scene_document_prompt_open()) {
    return;
  }

  constexpr const char *kPopupId = "Unsaved Changes###scene_unsaved_prompt";
  if (!ImGui::IsPopupOpen(kPopupId)) {
    ImGui::OpenPopup(kPopupId);
  }

  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  if (viewport != nullptr) {
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5F, 0.5F));
  }

  if (ImGui::BeginPopupModal(kPopupId, nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // The prompt names every document it stands for, so Save and Discard
    // read as decisions about exactly those documents.
    const bool sceneDirty = scene_document_is_dirty();
    if (scene_document_prompt_covers_material()) {
      const char *materialPath = material_editor_state().virtualPath;
      if (sceneDirty) {
        ImGui::Text("Save changes to \"%s\" and material \"%s\" before "
                    "quitting?",
                    scene_document_display_name(), materialPath);
      } else {
        ImGui::Text("Save changes to material \"%s\" before quitting?",
                    materialPath);
      }
    } else {
      ImGui::Text("Save changes to \"%s\" before continuing?",
                  scene_document_display_name());
    }
    const char *error = scene_document_last_error();
    if (error[0] != '\0') {
      ImGui::TextColored(ImVec4(0.9F, 0.35F, 0.35F, 1.0F), "%s", error);
    }

    if (ImGui::Button("Save")) {
      scene_document_prompt_choose_save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
      ImGui::CloseCurrentPopup();
      scene_document_prompt_choose_discard();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
      scene_document_prompt_choose_cancel();
    }

    if (!scene_document_prompt_open()) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

/// Draws one menu item per built-in primitive; the chosen one spawns at
/// the editor camera's focus point and becomes the selection.
static void draw_primitive_menu_items() noexcept {
  constexpr struct {
    const char *label;
    EditorPrimitive primitive;
  } kPrimitiveItems[] = {
      {"Cube", EditorPrimitive::Cube},
      {"Sphere", EditorPrimitive::Sphere},
      {"Cylinder", EditorPrimitive::Cylinder},
      {"Capsule", EditorPrimitive::Capsule},
      {"Pyramid", EditorPrimitive::Pyramid},
      {"Plane", EditorPrimitive::Plane},
  };
  const bool editable = world_is_editable();
  for (const auto &item : kPrimitiveItems) {
    if (ImGui::MenuItem(item.label, nullptr, false, editable)) {
      const runtime::Entity spawned = execute_primitive_spawn(item.primitive);
      if (spawned != runtime::kInvalidEntity) {
        select_entity(spawned, false);
      }
    }
  }
}

constexpr const char *kAboutPopupId = "About Engine";

/// Help > About: the engine version and the one-line build identity a bug
/// report should carry, with a button that copies it.
static void draw_about_popup() noexcept {
  if (!ImGui::BeginPopupModal(kAboutPopupId, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::Text("Engine %s", core::engine_version_string());
  ImGui::Separator();
  ImGui::TextUnformatted("Revision: " ENGINE_BUILD_DESCRIBE);
  ImGui::TextUnformatted("Compiler: " ENGINE_BUILD_COMPILER);
  ImGui::TextUnformatted("Configuration: " ENGINE_BUILD_TYPE);
  ImGui::TextUnformatted("Platform: " ENGINE_BUILD_PLATFORM);
  ImGui::TextUnformatted("Floating point: " ENGINE_BUILD_FLOAT);
  ImGui::Separator();
  if (ImGui::Button("Copy build info")) {
    ImGui::SetClipboardText(core::engine_build_id());
  }
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

/// The simulation speed presets Unity's and Unreal's play toolbars offer.
constexpr float kTimeScalePresets[] = {0.1F, 0.25F, 0.5F, 1.0F, 2.0F, 4.0F};

/// The toolbar's simulation speed: a combo over sim.time_scale, drawn
/// highlighted whenever the game does not run at real time so a slowed
/// session is never mistaken for a slow game. The value is not saved: a
/// new session always starts at 1.
static void draw_time_scale_combo() noexcept {
  const float scale = core::cvar_get_float("sim.time_scale", 1.0F);
  char preview[16] = {};
  std::snprintf(preview, sizeof(preview), "%gx", static_cast<double>(scale));
  const bool offRealTime = scale != 1.0F;
  if (offRealTime) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
                          ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
  }
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("0.25x").x +
                          (ImGui::GetStyle().FramePadding.x * 2.0F) +
                          ImGui::GetFrameHeight());
  if (ImGui::BeginCombo("##time_scale", preview)) {
    for (const float preset : kTimeScalePresets) {
      char label[16] = {};
      std::snprintf(label, sizeof(label), "%gx", static_cast<double>(preset));
      if (ImGui::Selectable(label, preset == scale)) {
        static_cast<void>(core::cvar_set_float("sim.time_scale", preset));
      }
    }
    ImGui::EndCombo();
  }
  if (offRealTime) {
    ImGui::PopStyleColor();
  }
  ImGui::SetItemTooltip("Simulation speed (sim.time_scale)");
}

void draw_main_menu_bar() noexcept {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  if (ImGui::BeginMenu("File")) {
    // Every item's label, chord and enabled state come from the action
    // table. Replacing or exporting the world stays available after a
    // failed Stop restore; only saving in place needs the editable world.
    editor_action_menu_item(EditorAction::NewScene);
    editor_action_menu_item(EditorAction::OpenScene);

    const std::size_t recentCount = recent_scene_count();
    if (ImGui::BeginMenu("Recent Scenes",
                         world_can_load_scene() && (recentCount > 0U))) {
      for (std::size_t i = 0U; i < recentCount; ++i) {
        const char *path = recent_scene_at(i);
        const std::string label =
            std::filesystem::path(path).filename().string();
        if (ImGui::MenuItem(label.empty() ? path : label.c_str())) {
          request_scene_open(path);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", path);
        }
      }
      ImGui::EndMenu();
    }

    ImGui::Separator();
    editor_action_menu_item(EditorAction::SaveScene);
    editor_action_menu_item(EditorAction::SaveSceneAs);
    ImGui::Separator();
    editor_action_menu_item(EditorAction::Exit);

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    editor_action_menu_item(EditorAction::Undo);
    editor_action_menu_item(EditorAction::Redo);
    ImGui::Separator();
    editor_action_menu_item(EditorAction::Copy);
    editor_action_menu_item(EditorAction::Paste);
    editor_action_menu_item(EditorAction::PasteAsChild);
    editor_action_menu_item(EditorAction::Duplicate);
    editor_action_menu_item(EditorAction::Delete);
    ImGui::Separator();
    editor_action_menu_item(EditorAction::FrameSelected);
    ImGui::Separator();
    // The play item reads Play or Stop; Pause is checked while it holds.
    const PlayState state = editor_session().playState;
    editor_action_menu_item(EditorAction::PlayStop);
    editor_action_menu_item(EditorAction::Pause, state == PlayState::Paused);
    editor_action_menu_item(EditorAction::Step);
    ImGui::Separator();
    editor_action_menu_item(EditorAction::Screenshot);
    ImGui::Separator();
    // Preferences sit under Edit, as in Unity; no reference editor has a
    // top-level Settings menu.
    const bool showPreferences =
        core::cvar_get_bool("editor.show_preferences", false);
    if (ImGui::MenuItem("Preferences...", nullptr, showPreferences)) {
      core::cvar_set_bool("editor.show_preferences", !showPreferences);
    }
    ImGui::EndMenu();
  }

  // Unity's GameObject menu: what the Entities panel's buttons create.
  if (ImGui::BeginMenu("Entity")) {
    editor_action_menu_item(EditorAction::CreateEmpty);
    if (ImGui::BeginMenu("3D Object", world_is_editable())) {
      draw_primitive_menu_items();
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Window")) {
    bool showLog = core::cvar_get_bool("editor.show_log", true);
    if (ImGui::MenuItem("Log", nullptr, showLog)) {
      core::cvar_set_bool("editor.show_log", !showLog);
    }
    const bool showRendering =
        core::cvar_get_bool("editor.show_rendering", false);
    if (ImGui::MenuItem("Rendering", nullptr, showRendering)) {
      core::cvar_set_bool("editor.show_rendering", !showRendering);
    }
    const bool showProfiler = core::cvar_get_bool(kShowProfilerCvar, false);
    if (ImGui::MenuItem("Profiler", nullptr, showProfiler)) {
      core::cvar_set_bool(kShowProfilerCvar, !showProfiler);
    }
    ImGui::EndMenu();
  }

  bool openAbout = false;
  if (ImGui::BeginMenu("Help")) {
    openAbout = ImGui::MenuItem("About");
    ImGui::EndMenu();
  }
  if (openAbout) {
    ImGui::OpenPopup(kAboutPopupId);
  }
  draw_about_popup();

  // Right-aligned in the menu bar: unseen Log warnings and errors
  // (nothing while all is well), then the document status: name plus a
  // dirty marker; scene_document_update_window_title mirrors the same
  // state into the OS title bar once per frame. A failed save stands
  // beside it until the next save succeeds: File > Save As opens no
  // prompt, so this is where its refusal is seen.
  char status[160] = {};
  std::snprintf(status, sizeof(status), "%s%s", scene_document_display_name(),
                scene_document_is_dirty() ? " *" : "");
  const char *saveError = scene_document_last_error();
  // Every gap and margin comes from the style, so the group stays inside
  // the bar at any UI scale.
  const ImGuiStyle &style = ImGui::GetStyle();
  const float gap = style.ItemSpacing.x * 2.0F;
  const float consoleWidth = console_status_indicator_width();
  const float errorWidth =
      (saveError[0] != '\0') ? ImGui::CalcTextSize(saveError).x : 0.0F;
  float groupWidth = ImGui::CalcTextSize(status).x;
  groupWidth += (consoleWidth > 0.0F) ? (consoleWidth + gap) : 0.0F;
  groupWidth += (errorWidth > 0.0F) ? (errorWidth + gap) : 0.0F;
  ImGui::SameLine(ImGui::GetWindowWidth() - groupWidth - style.WindowPadding.x);
  if (consoleWidth > 0.0F) {
    draw_console_status_indicator();
    ImGui::SameLine(0.0F, gap);
  }
  if (saveError[0] != '\0') {
    ImGui::TextColored(ImVec4(0.9F, 0.35F, 0.35F, 1.0F), "%s", saveError);
    ImGui::SameLine(0.0F, gap);
  }
  ImGui::TextUnformatted(status);

  ImGui::EndMainMenuBar();

  draw_unsaved_changes_prompt();
}

/// A toolbar button that runs `action` through the action table: its live
/// label, disabled when the action cannot run, drawn pressed while
/// `pressed`, with the purpose and the shortcut in its tooltip.
void toolbar_action_button(EditorAction action, bool pressed,
                           const char *purpose) noexcept {
  const bool enabled = editor_action_enabled(action);
  if (!enabled) {
    ImGui::BeginDisabled();
  }
  if (pressed) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  // The ### id keeps the button's identity while its label changes.
  char label[64] = {};
  std::snprintf(label, sizeof(label), "%s###toolbar_%d",
                editor_action_label(action), static_cast<int>(action));
  if (ImGui::Button(label) && enabled) {
    static_cast<void>(run_editor_action(action));
  }
  if (pressed) {
    ImGui::PopStyleColor();
  }
  if (!enabled) {
    ImGui::EndDisabled();
  }
  const char *chord = editor_shortcut_text(action);
  if (chord[0] != '\0') {
    ImGui::SetItemTooltip("%s (%s)", purpose, chord);
  } else {
    ImGui::SetItemTooltip("%s", purpose);
  }
}

void draw_toolbar() noexcept {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  if (viewport == nullptr) {
    return;
  }

  const float menuBarHeight = ImGui::GetFrameHeight();
  const float toolbarHeight = ImGui::GetFrameHeightWithSpacing();

  ImGui::SetNextWindowPos(
      ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight));
  ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, toolbarHeight));
  ImGui::SetNextWindowViewport(viewport->ID);

  constexpr ImGuiWindowFlags kToolbarFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;

  // While a session runs the toolbar takes an accent tint, the global
  // cue Unity's Playmode tint gives: edits made now are reverted on Stop.
  const bool running = editor_session().playState != PlayState::Stopped;
  if (running) {
    const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const ImVec4 accent(0.20F, 0.38F, 0.70F, base.w);
    constexpr float kTint = 0.45F;
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(base.x + ((accent.x - base.x) * kTint),
                                 base.y + ((accent.y - base.y) * kTint),
                                 base.z + ((accent.z - base.z) * kTint),
                                 base.w));
  }
  const bool open = ImGui::Begin("##toolbar", nullptr, kToolbarFlags);
  if (running) {
    ImGui::PopStyleColor();
  }
  if (!open) {
    ImGui::End();
    return;
  }

  const bool hasWorld = (editor_session().world != nullptr);
  const bool canPlay = hasWorld && !editor_session().worldRestoreFailed &&
                       (editor_session().playState != PlayState::Playing);

  // One-shot automation hook: the editor.autoplay cvar enters play mode on
  // the first eligible frame (scripted verification runs seed it with
  // ENGINE_CVAR_editor_autoplay=1; interactive sessions never set it). The
  // latch lives on the session so a later editor session in the same
  // process re-arms.
  if (!editor_session().autoplayConsumed && canPlay &&
      (editor_session().playState == PlayState::Stopped)) {
    editor_session().autoplayConsumed = true;
    if (core::cvar_get_bool("editor.autoplay", false)) {
      start_play_mode();
    }
  }
  // The play controls run the same actions as their shortcuts and the
  // Edit menu, so the three can never disagree. Play is one toggle that
  // reads Stop while a session runs, and Pause and Play are drawn pressed
  // while they hold, as Unity's toolbar is; Step works while playing too,
  // pausing first.
  const PlayState state = editor_session().playState;
  toolbar_action_button(EditorAction::PlayStop, state != PlayState::Stopped,
                        (state == PlayState::Stopped)
                            ? "Enter play mode"
                            : "Leave play mode; changes made while playing "
                              "are reverted");
  ImGui::SameLine();
  toolbar_action_button(EditorAction::Pause, state == PlayState::Paused,
                        (state == PlayState::Paused) ? "Resume"
                                                     : "Pause the game");
  ImGui::SameLine();
  toolbar_action_button(EditorAction::Step, false,
                        "Advance one fixed step, pausing first");

  ImGui::SameLine();
  draw_time_scale_combo();

  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  ImGui::SameLine();

  if (ImGui::RadioButton("T",
                         editor_session().gizmoOp == ImGuizmo::TRANSLATE)) {
    editor_session().gizmoOp = ImGuizmo::TRANSLATE;
  }
  ImGui::SetItemTooltip("Move (%s)",
                        editor_shortcut_text(EditorAction::GizmoTranslate));
  ImGui::SameLine();
  if (ImGui::RadioButton("R", editor_session().gizmoOp == ImGuizmo::ROTATE)) {
    editor_session().gizmoOp = ImGuizmo::ROTATE;
  }
  ImGui::SetItemTooltip("Rotate (%s)",
                        editor_shortcut_text(EditorAction::GizmoRotate));
  ImGui::SameLine();
  if (ImGui::RadioButton("S", editor_session().gizmoOp == ImGuizmo::SCALE)) {
    editor_session().gizmoOp = ImGuizmo::SCALE;
  }
  ImGui::SetItemTooltip("Scale (%s)",
                        editor_shortcut_text(EditorAction::GizmoScale));
  ImGui::SameLine();
  // Scale always works on the entity's own axes (ImGuizmo forces it: a
  // scale along a world axis would shear a rotated entity), so the toggle
  // shows Local while Scale is active and says why.
  const bool scaling = editor_session().gizmoOp == ImGuizmo::SCALE;
  const bool worldAxes = editor_session().gizmoWorldSpace && !scaling;
  if (ImGui::Button(worldAxes ? "World" : "Local")) {
    static_cast<void>(run_editor_action(EditorAction::GizmoSpace));
  }
  ImGui::SetItemTooltip(scaling ? "Scale always uses the entity's own axes "
                                  "(%s switches move and rotate)"
                                : "Handle axes: the world's or the entity's "
                                  "own (%s)",
                        editor_shortcut_text(EditorAction::GizmoSpace));

  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  ImGui::SameLine();
  if (ImGui::Checkbox("Grid", &editor_session().showGrid)) {
    ImGui::MarkIniSettingsDirty(); // a saved preference
  }
  ImGui::SetItemTooltip("The Scene view's ground grid; its spacing follows "
                        "the zoom");
  ImGui::SameLine();
  bool showStats = core::cvar_get_bool(kShowStatsCvar, false);
  if (ImGui::Checkbox("Stats", &showStats)) {
    static_cast<void>(core::cvar_set_bool(kShowStatsCvar, showStats));
    ImGui::MarkIniSettingsDirty(); // a saved preference
  }
  ImGui::SetItemTooltip("Frame rate, draw calls and memory over the Game "
                        "view; Window > Profiler has the detail");
  ImGui::SameLine();
  // Text-fitted widths follow the font, so the fields hold their widest
  // value at every UI scale.
  const float framePadding = ImGui::GetStyle().FramePadding.x * 2.0F;
  ImGui::TextUnformatted("Speed");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("100.00 m/s").x + framePadding);
  float &flySpeed = editor_session().editorCamera.flySpeed;
  if (ImGui::DragFloat(
          "##FlySpeed", &flySpeed, 0.05F, EditorCamera::kMinFlySpeed,
          EditorCamera::kMaxFlySpeed, "%.2f m/s",
          ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
    ImGui::MarkIniSettingsDirty();
  }
  ImGui::SetItemTooltip("Fly speed: hold the right mouse button in the "
                        "Scene view and use WASD, Q/E; Shift is four times "
                        "as fast, the wheel changes the speed");
  ImGui::SameLine();
  ImGui::Checkbox("Snap", &editor_session().snapEnabled);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("90 deg").x + framePadding);
  if (editor_session().gizmoOp == ImGuizmo::ROTATE) {
    ImGui::DragFloat("##SnapStep", &editor_session().snapAngleDegrees, 1.0F,
                     1.0F, 90.0F, "%.0f deg");
  } else {
    ImGui::DragFloat("##SnapStep", &editor_session().snapStep, 0.05F, 0.05F,
                     10.0F, "%.2f");
  }

  ImGui::End();
}

/// Hard bound on hierarchy tree nesting drawn per frame; deeper nodes
/// render as leaves so corrupted or absurdly deep parent chains cannot
/// grow the render call stack without limit.
constexpr std::size_t kMaxHierarchyDrawDepth = 64U;

/// An edit a hierarchy row asked for. It is applied once the walk ends:
/// the walk follows the world's child links, which an edit made mid-walk
/// would rewrite under it.
struct PendingHierarchyEdit final {
  enum class Kind : std::uint8_t { None, Action, Reparent };
  Kind kind = Kind::None;
  EditorAction action = EditorAction::Count;
  runtime::Entity target{};
  runtime::Entity newParent{};
};

/// Draws one hierarchy row with selection, its context menu and drag-drop
/// reparenting; returns whether its tree node is open.
static bool draw_entity_row(runtime::Entity entity, bool hasChildren,
                            PendingHierarchyEdit &pending) noexcept {
  char label[160] = {};
  runtime::NameComponent name{};
  if (editor_session().world->get_name_component(entity, &name) &&
      (name.name[0] != '\0')) {
    std::snprintf(label, sizeof(label), "%s###entity_%u", name.name,
                  entity.index);
  } else {
    std::snprintf(label, sizeof(label), "Entity [%u]###entity_%u", entity.index,
                  entity.index);
  }

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_SpanAvailWidth |
                             ImGuiTreeNodeFlags_DefaultOpen;
  if (!hasChildren) {
    flags |= ImGuiTreeNodeFlags_Leaf;
  }
  if (is_entity_selected(entity) || (selected_entity() == entity)) {
    flags |= ImGuiTreeNodeFlags_Selected;
  }

  const bool open = ImGui::TreeNodeEx(label, flags);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsItemToggledOpen()) {
    select_entity(entity, ImGui::GetIO().KeyCtrl);
  }
  // A double-click frames the row in the Scene view, as in Unity.
  if (ImGui::IsItemHovered() &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsItemToggledOpen()) {
    select_entity(entity, false);
    pending.kind = PendingHierarchyEdit::Kind::Action;
    pending.action = EditorAction::FrameSelected;
  }

  if (ImGui::BeginPopupContextItem(label)) {
    // A right-click selects the row first, so the actions below and the
    // Edit menu's act on the same entity.
    if (!is_entity_selected(entity) && (selected_entity() != entity)) {
      select_entity(entity, false);
    }
    // Each acts on the selection, which the right-click just made include
    // this row; Paste As Child pastes under it.
    for (const EditorAction action :
         {EditorAction::Copy, EditorAction::Paste, EditorAction::PasteAsChild,
          EditorAction::Duplicate, EditorAction::Delete}) {
      if (editor_action_menu_item_clicked(action)) {
        pending.kind = PendingHierarchyEdit::Kind::Action;
        pending.action = action;
      }
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
    ImGui::SetDragDropPayload("ENTITY_INDEX", &entity.index,
                              sizeof(entity.index));
    ImGui::TextUnformatted(label);
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload("ENTITY_INDEX")) {
      const std::uint32_t droppedIndex =
          *static_cast<const std::uint32_t *>(payload->Data);
      const runtime::Entity dropped =
          editor_session().world->find_entity_by_index(droppedIndex);
      if ((dropped != runtime::kInvalidEntity) && (dropped != entity) &&
          world_is_editable()) {
        pending.kind = PendingHierarchyEdit::Kind::Reparent;
        pending.target = dropped;
        pending.newParent = entity;
      }
    }
    ImGui::EndDragDropTarget();
  }
  return open;
}

/// Draws every root entity as a tree, then applies the edit a row asked
/// for.
static void draw_entity_hierarchy() noexcept {
  PendingHierarchyEdit pending{};
  walk_entity_hierarchy(
      *editor_session().world, kMaxHierarchyDrawDepth,
      [&pending](runtime::Entity entity, std::size_t,
                 bool hasChildren) noexcept {
        return draw_entity_row(entity, hasChildren, pending);
      },
      [](runtime::Entity) noexcept { ImGui::TreePop(); });

  switch (pending.kind) {
  case PendingHierarchyEdit::Kind::Action:
    static_cast<void>(run_editor_action(pending.action));
    break;
  case PendingHierarchyEdit::Kind::Reparent:
    static_cast<void>(execute_reparent(pending.target, pending.newParent));
    break;
  case PendingHierarchyEdit::Kind::None:
  default:
    break;
  }
}

void same_line_if_button_fits(const char *nextButtonLabel) noexcept {
  const ImGuiWindow *window = ImGui::GetCurrentWindow();
  if ((window == nullptr) || (nextButtonLabel == nullptr)) {
    return;
  }
  const ImGuiStyle &style = ImGui::GetStyle();
  const float buttonWidth =
      ImGui::CalcTextSize(nextButtonLabel, nullptr, true).x +
      (style.FramePadding.x * 2.0F);
  const float nextLeft = ImGui::GetItemRectMax().x + style.ItemSpacing.x;
  if (nextLeft + buttonWidth <= window->WorkRect.Max.x) {
    ImGui::SameLine();
  }
}

void draw_entities_panel() noexcept {
  if (!ImGui::Begin("Entities")) {
    ImGui::End();
    return;
  }

  if (editor_session().world == nullptr) {
    ImGui::TextUnformatted("No world attached");
    ImGui::End();
    return;
  }

  prune_entity_selection();
  draw_entity_hierarchy();

  // Dropping onto the panel background clears the parent.
  ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, editor_px(24.0F)));
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload("ENTITY_INDEX")) {
      const std::uint32_t droppedIndex =
          *static_cast<const std::uint32_t *>(payload->Data);
      const runtime::Entity dropped =
          editor_session().world->find_entity_by_index(droppedIndex);
      if ((dropped != runtime::kInvalidEntity) && world_is_editable()) {
        static_cast<void>(execute_reparent(dropped, runtime::kInvalidEntity));
      }
    }
    ImGui::EndDragDropTarget();
  }

  ImGui::Separator();
  const bool editable = world_is_editable();
  if (!editable) {
    ImGui::BeginDisabled();
  }

  if (ImGui::Button("Create Entity") && editable) {
    const runtime::Entity newEntity = execute_entity_create();
    if (newEntity != runtime::kInvalidEntity) {
      select_entity(newEntity, false);
    }
  }

  same_line_if_button_fits("Add Primitive");
  if (ImGui::Button("Add Primitive") && editable) {
    ImGui::OpenPopup("AddPrimitivePopup");
  }
  if (ImGui::BeginPopup("AddPrimitivePopup")) {
    draw_primitive_menu_items();
    ImGui::EndPopup();
  }

  if (!editable) {
    ImGui::EndDisabled();
  }

  ImGui::End();
}

} // namespace engine::editor
