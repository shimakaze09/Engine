// Implements the editor main menu bar, play toolbar, and entity hierarchy
// panel. Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_main.h"

#include "editor_commands.h"
#include "editor_panels_console.h"
#include "editor_scene_document.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
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

#include "engine/core/platform.h"
#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
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
#include "engine/editor/debug_camera.h"

#include <stb_image.h>

namespace engine::editor {

/// Draws the Save/Discard/Cancel confirm modal (issue #158) that gates
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
    ImGui::Text("Save changes to \"%s\" before continuing?",
               scene_document_display_name());
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

void draw_main_menu_bar() noexcept {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  if (ImGui::BeginMenu("File")) {
    const bool editable = world_is_editable();
    if (!editable) {
      ImGui::BeginDisabled();
    }

    if (ImGui::MenuItem("New Scene")) {
      request_scene_new();
    }
    if (ImGui::MenuItem("Open Scene...")) {
      request_open_scene_dialog();
    }

    const std::size_t recentCount = recent_scene_count();
    if (ImGui::BeginMenu("Recent Scenes", recentCount > 0U)) {
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

    if (ImGui::MenuItem("Save", "Ctrl+S")) {
      request_save_scene();
    }
    if (ImGui::MenuItem("Save As...")) {
      request_save_scene_as();
    }

    if (!editable) {
      ImGui::EndDisabled();
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    const bool canUndo =
        world_is_editable() && editor_session().commandHistory.can_undo();
    const bool canRedo =
        world_is_editable() && editor_session().commandHistory.can_redo();
    if (!canUndo) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
      editor_history_undo();
    }
    if (!canUndo) {
      ImGui::EndDisabled();
    }

    if (!canRedo) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z")) {
      editor_history_redo();
    }
    if (!canRedo) {
      ImGui::EndDisabled();
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Window")) {
    bool showConsole = core::cvar_get_bool("editor.show_console", true);
    if (ImGui::MenuItem("Console", nullptr, showConsole)) {
      core::cvar_set_bool("editor.show_console", !showConsole);
    }
    ImGui::EndMenu();
  }

  // Non-spamming status indicator (issue #155): Fatal/high-severity errors
  // stay visible in the menu bar even while the Console panel is closed.
  draw_console_status_indicator();

  // Document status (issue #158): name plus a dirty marker, right-aligned
  // in the menu bar; scene_document_update_window_title mirrors the same
  // state into the OS title bar once per frame.
  char status[160] = {};
  std::snprintf(status, sizeof(status), "%s%s", scene_document_display_name(),
               scene_document_is_dirty() ? " *" : "");
  const float statusWidth = ImGui::CalcTextSize(status).x;
  ImGui::SameLine(ImGui::GetWindowWidth() - statusWidth - 16.0F);
  ImGui::TextUnformatted(status);

  ImGui::EndMainMenuBar();

  draw_unsaved_changes_prompt();
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

  if (!ImGui::Begin("##toolbar", nullptr, kToolbarFlags)) {
    ImGui::End();
    return;
  }

  const bool hasWorld = (editor_session().world != nullptr);
  const bool canPlay = hasWorld && !editor_session().worldRestoreFailed &&
                       (editor_session().playState != PlayState::Playing);

  // One-shot automation hook: ENGINE_EDITOR_AUTOPLAY=1 enters play mode on
  // the first eligible frame (scripted verification runs use it; interactive
  // sessions never set the variable). The latch lives on the session
  // (#249) so a later editor session in the same process re-arms.
  if (!editor_session().autoplayConsumed && canPlay &&
      (editor_session().playState == PlayState::Stopped)) {
    const char *autoplay = core::non_empty_env("ENGINE_EDITOR_AUTOPLAY");
    editor_session().autoplayConsumed = true;
    if ((autoplay != nullptr) && (autoplay[0] == '1')) {
      start_play_mode();
    }
  }
  const bool canPause =
      hasWorld && (editor_session().playState == PlayState::Playing);
  const bool canStop =
      hasWorld && (editor_session().playState != PlayState::Stopped);

  if (!canPlay) {
    ImGui::BeginDisabled();
  }
  // Plain-text labels: the default ImGui font has no glyphs for the
  // media-control symbols (they render as "?").
  if (ImGui::Button("Play") && canPlay) {
    start_play_mode();
  }
  if (!canPlay) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (!canPause) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Pause") && canPause) {
    pause_play_mode();
  }
  if (!canPause) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  const bool canStep =
      hasWorld && (editor_session().playState == PlayState::Paused);
  if (!canStep) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Step") && canStep) {
    editor_session().stepRequested = true;
  }
  if (!canStep) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (!canStop) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Stop") && canStop) {
    stop_play_mode();
  }
  if (!canStop) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  ImGui::SameLine();

  if (ImGui::RadioButton("T",
                         editor_session().gizmoOp == ImGuizmo::TRANSLATE)) {
    editor_session().gizmoOp = ImGuizmo::TRANSLATE;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("R", editor_session().gizmoOp == ImGuizmo::ROTATE)) {
    editor_session().gizmoOp = ImGuizmo::ROTATE;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("S", editor_session().gizmoOp == ImGuizmo::SCALE)) {
    editor_session().gizmoOp = ImGuizmo::SCALE;
  }

  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  ImGui::SameLine();
  ImGui::Checkbox("Snap", &editor_session().snapEnabled);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(64.0F);
  if (editor_session().gizmoOp == ImGuizmo::ROTATE) {
    ImGui::DragFloat("##SnapStep", &editor_session().snapAngleDegrees, 1.0F,
                     1.0F, 90.0F, "%.0f deg");
  } else {
    ImGui::DragFloat("##SnapStep", &editor_session().snapStep, 0.05F, 0.05F,
                     10.0F, "%.2f");
  }

  ImGui::End();
}

/// True when the entity's transform names parentId as its parent (or the
/// entity has no transform and parentId is invalid, keeping
/// transform-less entities visible at the root).
static bool entity_has_parent(runtime::Entity entity,
                              runtime::PersistentId parentId) noexcept {
  runtime::Transform transform{};
  if (!editor_session().world->get_transform(entity, &transform)) {
    return parentId == runtime::kInvalidPersistentId;
  }
  return transform.parentId == parentId;
}

/// Hard bound on hierarchy tree nesting drawn per frame; deeper nodes
/// render as leaves so corrupted or absurdly deep parent chains cannot
/// grow the render call stack without limit.
constexpr std::size_t kMaxHierarchyDrawDepth = 64U;

/// Draws one hierarchy node with selection, drag-drop reparenting, and
/// its children as a subtree (depth-capped by kMaxHierarchyDrawDepth).
static void draw_entity_node(runtime::Entity entity,
                             std::size_t depth) noexcept {
  char label[160] = {};
  runtime::NameComponent name{};
  if (editor_session().world->get_name_component(entity, &name) &&
      (name.name[0] != '\0')) {
    std::snprintf(label, sizeof(label), "%s###entity_%u", name.name,
                  entity.index);
  } else {
    std::snprintf(label, sizeof(label), "Entity [%u]###entity_%u",
                  entity.index, entity.index);
  }

  const runtime::PersistentId ownId =
      editor_session().world->persistent_id(entity);
  bool hasChildren = false;
  if ((depth < kMaxHierarchyDrawDepth) &&
      (ownId != runtime::kInvalidPersistentId)) {
    editor_session().world->for_each_alive([&](runtime::Entity candidate) {
      if (!hasChildren && (candidate != entity) &&
          entity_has_parent(candidate, ownId)) {
        hasChildren = true;
      }
    });
  }

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_SpanAvailWidth |
                             ImGuiTreeNodeFlags_DefaultOpen;
  if (!hasChildren) {
    flags |= ImGuiTreeNodeFlags_Leaf;
  }
  if (is_entity_selected(entity) ||
      (selected_entity() == entity)) {
    flags |= ImGuiTreeNodeFlags_Selected;
  }

  const bool open = ImGui::TreeNodeEx(label, flags);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsItemToggledOpen()) {
    select_entity(entity, ImGui::GetIO().KeyCtrl);
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
        static_cast<void>(execute_reparent(dropped, entity));
      }
    }
    ImGui::EndDragDropTarget();
  }

  if (open) {
    if (hasChildren) {
      editor_session().world->for_each_alive([&](runtime::Entity candidate) {
        if ((candidate != entity) && entity_has_parent(candidate, ownId)) {
          draw_entity_node(candidate, depth + 1U);
        }
      });
    }
    ImGui::TreePop();
  }
}

/// Draws every root entity (no transform parent) as a tree.
static void draw_entity_hierarchy() noexcept {
  editor_session().world->for_each_alive([](runtime::Entity entity) {
    if (entity_has_parent(entity, runtime::kInvalidPersistentId)) {
      draw_entity_node(entity, 0U);
    }
  });
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
  ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 24.0F));
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

  ImGui::SameLine();
  if (ImGui::Button("Add Primitive") && editable) {
    ImGui::OpenPopup("AddPrimitivePopup");
  }
  if (ImGui::BeginPopup("AddPrimitivePopup")) {
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
    for (const auto &item : kPrimitiveItems) {
      if (ImGui::MenuItem(item.label) && editable) {
        const runtime::Entity spawned = execute_primitive_spawn(item.primitive);
        if (spawned != runtime::kInvalidEntity) {
          select_entity(spawned, false);
        }
      }
    }
    ImGui::EndPopup();
  }

  if (!editable) {
    ImGui::EndDisabled();
  }

  ImGui::End();
}

} // namespace engine::editor
