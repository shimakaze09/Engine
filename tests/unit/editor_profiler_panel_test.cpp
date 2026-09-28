// Verifies the editor's stats surfaces start out of the way and each
// appears on its own toggle: as registered, the overlay's r_showStats and
// the Profiler window's editor.show_profiler are off, a closed Profiler
// draws nothing, and setting its cvar draws the window.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <string>

#include "../test_harness.h"
#include "editor_panels_diagnostics.h"
#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"

namespace {

engine::tests::TestContext g_tests;

/// Draws one frame of the Profiler window from `stats`; true when the
/// window was submitted this frame.
bool profiler_frame(const engine::core::EngineStats &stats) noexcept {
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(640.0F, 700.0F));
  engine::editor::draw_profiler_panel(stats);
  ImGui::Render();
  const ImGuiWindow *window = ImGui::FindWindowByName("Profiler");
  return (window != nullptr) && window->Active;
}

} // namespace

int main() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 1.0F / 60.0F;
  io.IniFilename = nullptr;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  engine::editor::register_stats_cvars();
  g_tests.check(
      !engine::core::cvar_get_bool(engine::editor::kShowStatsCvar, true),
      "the stats overlay is off by default");
  g_tests.check(
      !engine::core::cvar_get_bool(engine::editor::kShowProfilerCvar, true),
      "the Profiler window is closed by default");
  const engine::core::EngineStats stats{};
  g_tests.check(!profiler_frame(stats), "a closed Profiler draws nothing");

  g_tests.check(
      engine::core::cvar_set_bool(engine::editor::kShowProfilerCvar, true),
      "open the Profiler");
  g_tests.check(profiler_frame(stats), "an open Profiler draws its window");

  static_cast<void>(
      engine::core::cvar_set_bool(engine::editor::kShowProfilerCvar, false));
  g_tests.check(!profiler_frame(stats), "closing it again hides it");

  ImGui::DestroyContext();
  return g_tests.finish("editor_profiler_panel");
}
