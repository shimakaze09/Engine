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

/// Applies the editor's visual theme: neutral dark palette, one restrained
/// accent hue, soft rounding, and roomier spacing than ImGui's defaults.
void apply_editor_style() noexcept {
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();

  style.WindowRounding = 4.0F;
  style.ChildRounding = 4.0F;
  style.FrameRounding = 3.0F;
  style.PopupRounding = 3.0F;
  style.GrabRounding = 3.0F;
  style.TabRounding = 3.0F;
  style.ScrollbarRounding = 6.0F;
  style.WindowPadding = ImVec2(10.0F, 8.0F);
  style.FramePadding = ImVec2(8.0F, 4.0F);
  style.ItemSpacing = ImVec2(8.0F, 5.0F);
  style.ItemInnerSpacing = ImVec2(6.0F, 4.0F);
  style.IndentSpacing = 18.0F;
  style.ScrollbarSize = 12.0F;
  style.GrabMinSize = 10.0F;
  style.WindowBorderSize = 1.0F;
  style.FrameBorderSize = 0.0F;
  style.TabBarBorderSize = 1.0F;
  style.WindowMenuButtonPosition = ImGuiDir_None;

  ImVec4 *colors = style.Colors;
  const ImVec4 bg(0.106F, 0.113F, 0.125F, 1.0F);
  const ImVec4 bgDark(0.082F, 0.086F, 0.094F, 1.0F);
  const ImVec4 bgLight(0.145F, 0.153F, 0.169F, 1.0F);
  const ImVec4 frame(0.175F, 0.184F, 0.204F, 1.0F);
  const ImVec4 accent(0.257F, 0.469F, 0.700F, 1.0F);
  const ImVec4 accentHover(0.312F, 0.539F, 0.781F, 1.0F);
  const ImVec4 accentActive(0.211F, 0.406F, 0.622F, 1.0F);

  colors[ImGuiCol_WindowBg] = bg;
  colors[ImGuiCol_ChildBg] = bg;
  colors[ImGuiCol_PopupBg] = bgDark;
  colors[ImGuiCol_MenuBarBg] = bgDark;
  colors[ImGuiCol_TitleBg] = bgDark;
  colors[ImGuiCol_TitleBgActive] = bgLight;
  colors[ImGuiCol_TitleBgCollapsed] = bgDark;
  colors[ImGuiCol_FrameBg] = frame;
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22F, 0.23F, 0.26F, 1.0F);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.26F, 0.28F, 0.31F, 1.0F);
  colors[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.35F);
  colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55F);
  colors[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.75F);
  colors[ImGuiCol_Button] = frame;
  colors[ImGuiCol_ButtonHovered] = accentHover;
  colors[ImGuiCol_ButtonActive] = accentActive;
  colors[ImGuiCol_CheckMark] = accentHover;
  colors[ImGuiCol_SliderGrab] = accent;
  colors[ImGuiCol_SliderGrabActive] = accentHover;
  colors[ImGuiCol_Tab] = bgDark;
  colors[ImGuiCol_TabHovered] = accentHover;
  colors[ImGuiCol_TabSelected] = accent;
  colors[ImGuiCol_TabDimmed] = bgDark;
  colors[ImGuiCol_TabDimmedSelected] = bgLight;
  colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.6F);
  colors[ImGuiCol_SeparatorHovered] = accentHover;
  colors[ImGuiCol_SeparatorActive] = accent;
  colors[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.25F);
  colors[ImGuiCol_ResizeGripHovered] =
      ImVec4(accent.x, accent.y, accent.z, 0.6F);
  colors[ImGuiCol_ResizeGripActive] = accent;
  colors[ImGuiCol_ScrollbarBg] = bgDark;
  colors[ImGuiCol_ScrollbarGrab] = frame;
  colors[ImGuiCol_ScrollbarGrabHovered] = bgLight;
  colors[ImGuiCol_ScrollbarGrabActive] = accent;
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

  // Proper UI font (the 13px bitmap default reads as a debug tool). Falls
  // back to the built-in font when the asset is missing.
  const ImFont *editorFont = io.Fonts->AddFontFromFileTTF(
      "assets/fonts/Roboto-Medium.ttf", 17.0F);
  if (editorFont == nullptr) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "editor font missing; using ImGui default");
  }

  apply_editor_style();

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

void editor_render(float frameMs, float utilizationPct) noexcept {
  if (!editor_session().initialized) {
    return;
  }

  draw_editor_panels(frameMs, utilizationPct);
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void editor_process_event(void *sdlEvent) noexcept {
  if (!editor_session().initialized || (sdlEvent == nullptr)) {
    return;
  }

  ImGui_ImplSDL2_ProcessEvent(static_cast<SDL_Event *>(sdlEvent));
}

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

bool editor_is_playing() noexcept { return editor_session().playState == PlayState::Playing; }

bool editor_is_paused() noexcept { return editor_session().playState == PlayState::Paused; }

namespace {

bool editor_wants_capture_keyboard() noexcept {
  if (!editor_session().initialized) {
    return false;
  }

  return ImGui::GetIO().WantCaptureKeyboard;
}

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
