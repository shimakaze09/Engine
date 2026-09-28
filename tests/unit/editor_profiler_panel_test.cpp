// Verifies the editor's stats surfaces start out of the way and each
// appears on its own toggle: as registered, the overlay's r_showStats and
// the Profiler window's editor.show_profiler are off, a closed Profiler
// draws nothing, and setting its cvar draws the window. An open Profiler
// names every memory subsystem in full (a fixed-width column used to cut
// them off) and says "not measured" for GPU timings the device cannot
// take instead of printing zeros.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <cstddef>
#include <string>

#include "../test_harness.h"
#include "editor_panels_diagnostics.h"
#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/mem_tracker.h"

namespace {

engine::tests::TestContext g_tests;

/// Draws one frame of the Profiler window from `stats`; true when the
/// window was submitted this frame. `text`, when given, receives what the
/// frame rendered as text.
bool profiler_frame(const engine::core::EngineStats &stats,
                    std::string *text = nullptr) noexcept {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(640.0F, 700.0F));
  engine::editor::draw_profiler_panel(stats);
  if (text != nullptr) {
    *text = GImGui->LogBuffer.c_str();
  }
  ImGui::LogFinish();
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

  std::string text;
  static_cast<void>(profiler_frame(stats, &text));
  std::array<engine::core::MemTagSnapshot, engine::core::kMemTagCount> snaps =
      std::array<engine::core::MemTagSnapshot, engine::core::kMemTagCount>();
  const std::size_t tagCount =
      engine::core::mem_tracker_snapshot(snaps.data(), snaps.size());
  bool everyName = tagCount > 0U;
  for (std::size_t i = 0U; i < tagCount; ++i) {
    everyName =
        everyName && (text.find(engine::core::mem_tag_name(snaps[i].tag)) !=
                      std::string::npos);
  }
  g_tests.check(everyName, "every memory subsystem is named");
  // The names' column is as wide as the widest name, so no bar starts
  // over one.
  float widestName = 0.0F;
  for (std::size_t i = 0U; i < tagCount; ++i) {
    const float tagWidth =
        ImGui::CalcTextSize(engine::core::mem_tag_name(snaps[i].tag)).x;
    widestName = (tagWidth > widestName) ? tagWidth : widestName;
  }
  const ImGuiWindow *profiler = ImGui::FindWindowByName("Profiler");
  const ImGuiTable *memory =
      (profiler != nullptr)
          ? ImGui::TableFindByID(ImHashStr("##memory", 0, profiler->ID))
          : nullptr;
  g_tests.check((memory != nullptr) &&
                    (memory->Columns[0].WidthGiven >= widestName),
                "the memory names' column fits the widest name");
  g_tests.check(text.find("GPU scene") != std::string::npos,
                "the GPU rows are listed");
  g_tests.check(text.find("not measured") != std::string::npos,
                "unmeasurable GPU timings read \"not measured\"");

  engine::core::EngineStats measured{};
  measured.gpuTimingAvailable = true;
  measured.gpuSceneMs = 1.25F;
  static_cast<void>(profiler_frame(measured, &text));
  g_tests.check(text.find("1.250 ms") != std::string::npos,
                "a measured GPU timing is printed");

  static_cast<void>(
      engine::core::cvar_set_bool(engine::editor::kShowProfilerCvar, false));
  g_tests.check(!profiler_frame(stats), "closing it again hides it");

  ImGui::DestroyContext();
  return g_tests.finish("editor_profiler_panel");
}
