// Implements editor behavior for the Engine editor tool.

#include "engine/editor/editor.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#if __has_include(<SDL_opengl.h>)
#include <SDL_opengl.h>
#elif __has_include(<SDL2/SDL_opengl.h>)
#include <SDL2/SDL_opengl.h>
#else
#error "SDL OpenGL headers not found"
#endif

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
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

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/engine.h"
#include "engine/editor/editor_camera.h"
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

#include "editor_commands.h"
#include "editor_panels_assets.h"
#include "editor_panels_diagnostics.h"
#include "editor_panels_inspector.h"
#include "editor_panels_main.h"
#include "editor_panels_viewport.h"
#include "editor_session.h"

namespace engine::editor {

namespace {

void setup_default_dock_layout(ImGuiID dockspaceId) noexcept {
  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

  ImGuiID center = dockspaceId;
  ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20F,
                                             nullptr, &center);
  ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25F,
                                              nullptr, &center);
  ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25F,
                                               nullptr, &center);

  ImGui::DockBuilderDockWindow("Entities", left);
  ImGui::DockBuilderDockWindow("Inspector", right);
  ImGui::DockBuilderDockWindow("Stats", bottom);
  ImGui::DockBuilderDockWindow("Assets", bottom);
  ImGui::DockBuilderDockWindow("Scene", center);

  ImGui::DockBuilderFinish(dockspaceId);
}

/// Handles draw editor panels.
void draw_editor_panels(float frameMs, float utilizationPct) noexcept {
  static_cast<void>(frameMs);
  static_cast<void>(utilizationPct);

  draw_main_menu_bar();
  draw_toolbar();

  const bool showStats = core::cvar_get_bool("r_showStats", true);
  const core::EngineStats stats = core::get_engine_stats();

  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  if (viewport == nullptr) {
    return;
  }

  const float menuBarHeight = ImGui::GetFrameHeight();
  const float toolbarHeight = ImGui::GetFrameHeightWithSpacing();
  const float topOffset = menuBarHeight + toolbarHeight;

  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + topOffset));
  ImGui::SetNextWindowSize(
      ImVec2(viewport->Size.x, viewport->Size.y - topOffset));
  ImGui::SetNextWindowViewport(viewport->ID);

  constexpr ImGuiWindowFlags kDockWindowFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
      ImGuiWindowFlags_NoBackground;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
  ImGui::Begin("##DockSpaceHost", nullptr, kDockWindowFlags);
  ImGui::PopStyleVar(3);

  const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");

  if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
    setup_default_dock_layout(dockspaceId);
  }

  ImGui::DockSpace(dockspaceId, ImVec2(0.0F, 0.0F), ImGuiDockNodeFlags_None);
  ImGui::End();

  draw_scene_viewport_panel();
  draw_entities_panel();
  draw_inspector_panel();
  if (showStats) {
    draw_stats_panel(stats);
    draw_in_game_stats_overlay(stats);
  }
  draw_asset_browser_panel();
}

} // namespace

/// Initializes the owning system for editor.
bool initialize_editor(void *sdlWindow, void *glContext) noexcept {
  if (editor_session().initialized) {
    return true;
  }

  if ((sdlWindow == nullptr) || (glContext == nullptr)) {
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  ImGui::StyleColorsDark();

  static_cast<void>(core::cvar_register_bool(
      "r_showStats", true,
      "Toggle in-game stats and profiling overlays in the editor"));

  static_cast<void>(core::cvar_register_bool(
      "debug.camera_detach", false,
      "Detach debug free-fly camera from game camera"));

  if (!ImGui_ImplSDL2_InitForOpenGL(static_cast<SDL_Window *>(sdlWindow),
                                    static_cast<SDL_GLContext>(glContext))) {
    ImGui::DestroyContext();
    return false;
  }

  if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    return false;
  }

  // Meaningful editor testing requires a live OpenGL context.
  editor_session().initialized = true;
  return true;
}

/// Shuts down the owning system for editor.
void shutdown_editor() noexcept {
  if (!editor_session().initialized) {
    return;
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  clear_thumbnail_cache();
  editor_session().commandHistory.clear();

  editor_session().initialized = false;
  editor_session().world = nullptr;
  editor_session().selectedEntityIndex = 0U;
  editor_session().playState = PlayState::Stopped;
  editor_session().playSnapshotBuffer.reset();
  editor_session().playSnapshotCapacity = 0U;
  editor_session().playSnapshotSize = 0U;
  editor_session().hasPlaySnapshot = false;
  editor_session().worldRestoreFailed = false;
}

/// Handles editor new frame.
void editor_new_frame() noexcept {
  if (!editor_session().initialized) {
    return;
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  ImGuizmo::BeginFrame();

  // Keyboard shortcuts.
  const ImGuiIO &io = ImGui::GetIO();
  if (!io.WantTextInput) {
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) {
      editor_session().commandHistory.undo();
    }
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) {
      editor_session().commandHistory.redo();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_W)) {
      editor_session().gizmoOp = ImGuizmo::TRANSLATE;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E)) {
      editor_session().gizmoOp = ImGuizmo::ROTATE;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
      editor_session().gizmoOp = ImGuizmo::SCALE;
    }
  }
}

/// Handles editor render.
void editor_render(float frameMs, float utilizationPct) noexcept {
  if (!editor_session().initialized) {
    return;
  }

  draw_editor_panels(frameMs, utilizationPct);
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

/// Handles editor process event.
void editor_process_event(void *sdlEvent) noexcept {
  if (!editor_session().initialized || (sdlEvent == nullptr)) {
    return;
  }

  ImGui_ImplSDL2_ProcessEvent(static_cast<SDL_Event *>(sdlEvent));
}

/// Handles editor set world.
void editor_set_world(runtime::World *world) noexcept {
  if (editor_session().world != world) {
    editor_session().commandHistory.clear();
    editor_session().gizmoWasUsing = false;
  }
  editor_session().world = world;
  if (world == nullptr) {
    editor_session().selectedEntityIndex = 0U;
    editor_session().playState = PlayState::Stopped;
    editor_session().playSnapshotSize = 0U;
    editor_session().hasPlaySnapshot = false;
    editor_session().worldRestoreFailed = false;
  }
}

/// Handles editor is playing.
bool editor_is_playing() noexcept { return editor_session().playState == PlayState::Playing; }

/// Handles editor is paused.
bool editor_is_paused() noexcept { return editor_session().playState == PlayState::Paused; }

namespace {

/// Handles editor wants capture keyboard.
bool editor_wants_capture_keyboard() noexcept {
  if (!editor_session().initialized) {
    return false;
  }

  return ImGui::GetIO().WantCaptureKeyboard;
}

/// Handles editor wants capture mouse.
bool editor_wants_capture_mouse() noexcept {
  if (!editor_session().initialized) {
    return false;
  }

  return ImGui::GetIO().WantCaptureMouse;
}

const runtime::EditorBridge kRuntimeEditorBridge = {
    &initialize_editor,
    &shutdown_editor,
    &editor_new_frame,
    &editor_render,
    &editor_process_event,
    &editor_set_world,
    &editor_is_playing,
    &editor_is_paused,
    &editor_wants_capture_keyboard,
    &editor_wants_capture_mouse,
};

[[maybe_unused]] const bool kEditorBridgeRegistered = []() noexcept {
  runtime::set_editor_bridge(&kRuntimeEditorBridge);
  return true;
}();

} // namespace

} // namespace engine::editor
