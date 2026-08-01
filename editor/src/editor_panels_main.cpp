// Implements the editor main menu bar, play toolbar, and entity hierarchy
// panel. Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_main.h"

#include "editor_commands.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include <SDL3/SDL_opengl.h>

#include "backends/imgui_impl_opengl3.h"
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

void draw_main_menu_bar() noexcept {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  if (ImGui::BeginMenu("File")) {
    const bool canSaveScene = world_is_editable();
    const bool sceneFileExists = default_scene_file_exists();
    const bool canLoadScene = world_can_load_scene() && sceneFileExists;

    if (!canSaveScene) {
      ImGui::BeginDisabled();
    }

    if (ImGui::MenuItem("Save Scene") && canSaveScene) {
      const char *scenePath = editor_scene_path();
      if (!runtime::save_scene(*editor_session().world, scenePath)) {
        core::log_message(core::LogLevel::Error, "editor",
                          "failed to save configured editor scene");
      }
    }

    if (!canSaveScene) {
      ImGui::EndDisabled();
    }

    if (!canLoadScene) {
      ImGui::BeginDisabled();
    }

    const bool loadScenePressed = ImGui::MenuItem("Load Scene");
    if (!sceneFileExists &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::BeginTooltip();
      ImGui::Text("No saved scene at %s", editor_scene_path());
      ImGui::EndTooltip();
    }

    if (loadScenePressed && canLoadScene) {
      const char *scenePath = editor_scene_path();
      if (!runtime::load_scene(*editor_session().world, scenePath)) {
        core::log_message(core::LogLevel::Error, "editor",
                          "failed to load configured editor scene");
      } else {
        editor_session().selectedEntityIndex = 0U;
        editor_session().worldRestoreFailed = false;
        editor_session().commandHistory.clear();
      }
    }

    if (!canLoadScene) {
      ImGui::EndDisabled();
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    if (!editor_session().commandHistory.can_undo()) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
      editor_session().commandHistory.undo();
    }
    if (!editor_session().commandHistory.can_undo()) {
      ImGui::EndDisabled();
    }

    if (!editor_session().commandHistory.can_redo()) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z")) {
      editor_session().commandHistory.redo();
    }
    if (!editor_session().commandHistory.can_redo()) {
      ImGui::EndDisabled();
    }

    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
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
  // sessions never set the variable).
  static bool autoplayConsumed = false;
  if (!autoplayConsumed && canPlay &&
      (editor_session().playState == PlayState::Stopped)) {
    const char *autoplay = core::non_empty_env("ENGINE_EDITOR_AUTOPLAY");
    autoplayConsumed = true;
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

/// Draws one hierarchy node with selection, drag-drop reparenting, and
/// its children as a subtree.
static void draw_entity_node(runtime::Entity entity) noexcept {
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
  if (ownId != runtime::kInvalidPersistentId) {
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
  if (is_entity_selected(entity.index) ||
      (editor_session().selectedEntityIndex == entity.index)) {
    flags |= ImGuiTreeNodeFlags_Selected;
  }

  const bool open = ImGui::TreeNodeEx(label, flags);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsItemToggledOpen()) {
    select_entity(entity.index, ImGui::GetIO().KeyCtrl);
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
          draw_entity_node(candidate);
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
      draw_entity_node(entity);
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
    const runtime::Entity newEntity =
        editor_session().world->create_scene_object();
    if (newEntity != runtime::kInvalidEntity) {
      runtime::NameComponent nameComponent{};
      make_default_entity_name(newEntity.index, &nameComponent);
      static_cast<void>(
          editor_session().world->add_name_component(newEntity, nameComponent));
      editor_session().selectedEntityIndex = newEntity.index;
    }
  }

  if (!editable) {
    ImGui::EndDisabled();
  }

  ImGui::End();
}

} // namespace engine::editor
