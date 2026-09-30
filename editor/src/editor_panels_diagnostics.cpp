// Implements the editor's Profiler window (frame numbers, CPU flame graph,
// memory by subsystem) and the stats overlay over the Game or Scene view.

#include "editor_panels_diagnostics.h"

#include "editor_commands.h"
#include "editor_frame_history.h"
#include "editor_session.h"

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
#include <vector>

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

#include <stb_image.h>

namespace engine::editor {

namespace {

void draw_profiler_flame_graph() noexcept {
  std::array<core::ProfileEntry, 256U> entries =
      std::array<core::ProfileEntry, 256U>();
  const std::size_t count =
      core::profiler_get_entries(entries.data(), entries.size());
  if (count == 0U) {
    ImGui::TextUnformatted("Profiler: no samples");
    return;
  }

  const float frameMs = core::profiler_frame_time_ms();
  const float graphMs = (frameMs > 0.001F) ? frameMs : 0.001F;
  const float graphWidth = ImGui::GetContentRegionAvail().x;
  // A bar is one line of text tall, so its label fits inside it.
  const float barHeight =
      ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y;
  const float barSpacing = editor_px(2.0F);
  // Narrower than this, a bar shows no label rather than a clipped stub.
  const float minLabelWidth = ImGui::CalcTextSize("MMM").x;

  std::array<float, 256U> startMs{};

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const ImVec2 graphOrigin = ImGui::GetCursorScreenPos();

  std::uint32_t maxDepth = 0U;
  static_cast<void>(core::profiler_compute_flame_starts(
      entries.data(), count, startMs.data(), &maxDepth));

  for (std::size_t i = 0U; i < count; ++i) {
    const core::ProfileEntry &entry = entries[i];
    const float thisStartMs = startMs[i];
    const float x0 = graphOrigin.x + (thisStartMs / graphMs) * graphWidth;
    const float x1 = x0 + (entry.durationMs / graphMs) * graphWidth;
    const float y0 = graphOrigin.y +
                     static_cast<float>(entry.depth) * (barHeight + barSpacing);
    const float y1 = y0 + barHeight;
    const int colorSeed =
        static_cast<int>((i * 37U + entry.depth * 19U) % 155U);
    const ImU32 color =
        IM_COL32(80 + colorSeed, 180, 240 - (colorSeed / 2), 220);
    drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color,
                            editor_px(2.0F));
    if ((x1 - x0) < minLabelWidth) {
      continue;
    }

    char label[96] = {};
    const char *name = (entry.name != nullptr) ? entry.name : "<unnamed>";
    std::snprintf(label, sizeof(label), "%s %.2fms", name,
                  static_cast<double>(entry.durationMs));
    // Clipped to its own bar, so a label never runs over its neighbours.
    drawList->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    drawList->AddText(ImVec2(x0 + editor_px(3.0F),
                             y0 + (ImGui::GetStyle().FramePadding.y * 0.5F)),
                      IM_COL32(0, 0, 0, 255), label);
    drawList->PopClipRect();
  }

  const float graphHeight =
      static_cast<float>(maxDepth + 1U) * (barHeight + barSpacing);
  ImGui::Dummy(ImVec2(graphWidth, graphHeight));
}

/// Reads one kept frame's value of a series for ImGui::PlotLines.
float plot_value(void *data, int index) noexcept {
  const FrameSeries series = *static_cast<const FrameSeries *>(data);
  return frame_history_value(series, static_cast<std::size_t>(index));
}

/// One series over the kept frames, as Unity's Profiler charts and
/// Unreal's stat graphs draw them: the line, its current, average and
/// maximum readouts, and, for frame time, guides at the 60 and 30 fps
/// budgets so a spike past one reads at a glance.
void draw_series_graph(const char *label, FrameSeries series, const char *unit,
                       bool frameBudgets) noexcept {
  FrameSeries plotted = series;
  const FrameSeriesSummary summary = frame_history_summary(series);
  char overlay[96] = {};
  std::snprintf(overlay, sizeof(overlay), "%.2f%s  avg %.2f  max %.2f",
                static_cast<double>(summary.current), unit,
                static_cast<double>(summary.average),
                static_cast<double>(summary.maximum));
  // The scale holds the tallest frame kept, and both budgets when they
  // are graphed, so the guides always sit inside the plot.
  float scaleMax = summary.maximum * 1.1F;
  if (frameBudgets && (scaleMax < 36.0F)) {
    scaleMax = 36.0F;
  }
  if (scaleMax <= 0.0F) {
    scaleMax = 1.0F;
  }
  ImGui::TextUnformatted(label);
  ImGui::PlotLines("##graph", &plot_value, &plotted,
                   static_cast<int>(frame_history_count()), 0, overlay, 0.0F,
                   scaleMax, ImVec2(-1.0F, ImGui::GetTextLineHeight() * 4.0F));
  if (!frameBudgets) {
    return;
  }
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  ImDrawList *drawList = ImGui::GetWindowDrawList();
  constexpr float kBudgetsMs[] = {1000.0F / 60.0F, 1000.0F / 30.0F};
  constexpr ImU32 kBudgetColors[] = {IM_COL32(120, 220, 120, 160),
                                     IM_COL32(240, 170, 80, 160)};
  for (std::size_t i = 0U; i < 2U; ++i) {
    const float y = max.y - ((kBudgetsMs[i] / scaleMax) * (max.y - min.y));
    drawList->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), kBudgetColors[i]);
  }
}

/// The kept frames as graphs, newest on the right.
void draw_frame_graphs() noexcept {
  ImGui::PushID("frame_graphs");
  ImGui::PushID(0);
  draw_series_graph("Frame time (ms; guides at 60 and 30 fps)",
                    FrameSeries::FrameMs, " ms", true);
  ImGui::PopID();
  ImGui::PushID(1);
  draw_series_graph("Draw calls", FrameSeries::DrawCalls, "", false);
  ImGui::PopID();
  ImGui::PushID(2);
  draw_series_graph("Memory (MB)", FrameSeries::MemoryMb, " MB", false);
  ImGui::PopID();
  ImGui::PushID(3);
  draw_series_graph("Job utilization (%)", FrameSeries::JobUtilizationPct, "%",
                    false);
  ImGui::PopID();
  ImGui::PopID();
}

/// One label and value row of a two-column table; a value the device
/// cannot measure reads "not measured" rather than a zero.
void stat_row(const char *label, const char *value, bool measured) noexcept {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  if (measured) {
    ImGui::TextUnformatted(value);
  } else {
    ImGui::TextDisabled("not measured");
  }
}

/// The frame's numbers as a label and value table.
void draw_frame_table(const core::EngineStats &stats) noexcept {
  if (!ImGui::BeginTable("##frame", 2, ImGuiTableFlags_SizingFixedFit)) {
    return;
  }
  char value[64] = {};
  std::snprintf(value, sizeof(value), "%.1f fps",
                static_cast<double>(stats.fps));
  stat_row("Frame rate", value, true);
  std::snprintf(value, sizeof(value), "%.3f ms",
                static_cast<double>(stats.frameTimeMs));
  stat_row("Frame time", value, true);
  std::snprintf(value, sizeof(value), "%u", stats.drawCalls);
  stat_row("Draw calls", value, true);
  std::snprintf(value, sizeof(value), "%llu",
                static_cast<unsigned long long>(stats.triCount));
  stat_row("Triangles", value, true);
  std::snprintf(value, sizeof(value), "%zu", stats.entityCount);
  stat_row("Entities", value, true);
  std::snprintf(value, sizeof(value), "%.2f MB",
                static_cast<double>(stats.memoryUsedMb));
  stat_row("Memory", value, true);
  std::snprintf(value, sizeof(value), "%.2f%%",
                static_cast<double>(stats.jobUtilizationPct));
  stat_row("Job utilization", value, true);
  std::snprintf(value, sizeof(value), "%.3f ms",
                static_cast<double>(stats.gpuSceneMs));
  stat_row("GPU scene", value, stats.gpuTimingAvailable);
  std::snprintf(value, sizeof(value), "%.3f ms",
                static_cast<double>(stats.gpuTonemapMs));
  stat_row("GPU tonemap", value, stats.gpuTimingAvailable);
  ImGui::EndTable();
}

/// Memory by subsystem: each tag's name in a column as wide as the widest
/// name, and its share of the largest as a bar.
void draw_memory_table() noexcept {
  std::array<core::MemTagSnapshot, core::kMemTagCount> snaps =
      std::array<core::MemTagSnapshot, core::kMemTagCount>();
  const std::size_t count =
      core::mem_tracker_snapshot(snaps.data(), snaps.size());
  float maxBytes = 1.0F;
  float nameWidth = 0.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    const float bytes = static_cast<float>(
        snaps[i].currentBytes > 0 ? snaps[i].currentBytes : 0);
    maxBytes = (bytes > maxBytes) ? bytes : maxBytes;
    const float width = ImGui::CalcTextSize(core::mem_tag_name(snaps[i].tag)).x;
    nameWidth = (width > nameWidth) ? width : nameWidth;
  }
  if (!ImGui::BeginTable("##memory", 2, ImGuiTableFlags_None)) {
    return;
  }
  ImGui::TableSetupColumn("Subsystem", ImGuiTableColumnFlags_WidthFixed,
                          nameWidth);
  ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch);
  for (std::size_t i = 0U; i < count; ++i) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(core::mem_tag_name(snaps[i].tag));
    ImGui::TableSetColumnIndex(1);
    if (!snaps[i].reported) {
      // Nothing reports under this tag: say so rather than draw a zero
      // that reads as a measurement.
      ImGui::TextDisabled("not measured");
      continue;
    }
    const float bytes = static_cast<float>(
        snaps[i].currentBytes > 0 ? snaps[i].currentBytes : 0);
    char label[64] = {};
    std::snprintf(label, sizeof(label), "%.2f MB",
                  static_cast<double>(bytes / (1024.0F * 1024.0F)));
    ImGui::ProgressBar(bytes / maxBytes, ImVec2(-1.0F, 0.0F), label);
  }
  ImGui::EndTable();
}

} // namespace

void register_stats_cvars() noexcept {
  static_cast<void>(core::cvar_register_bool(
      kShowStatsCvar, false,
      "Show the editor's stats overlay over the Game view (the Scene view "
      "while the Game view is hidden); the toolbar's Stats toggle"));
  static_cast<void>(core::cvar_register_bool(
      kShowProfilerCvar, false,
      "Toggle the editor Profiler window (Window menu)"));
}

void draw_profiler_panel(const core::EngineStats &stats) noexcept {
  if (!core::cvar_get_bool(kShowProfilerCvar, false)) {
    return;
  }
  // Opened beside the Log, where a layout without it has room.
  const ImGuiWindow *console = ImGui::FindWindowByName("Log");
  if ((console != nullptr) && (console->DockId != 0U)) {
    ImGui::SetNextWindowDockID(console->DockId, ImGuiCond_FirstUseEver);
  }
  bool open = true;
  const bool visible = ImGui::Begin("Profiler", &open);
  if (!open) {
    static_cast<void>(core::cvar_set_bool(kShowProfilerCvar, false));
  }
  if (!visible) {
    ImGui::End();
    return;
  }

  ImGui::SeparatorText("Frames");
  draw_frame_graphs();

  ImGui::SeparatorText("This frame");
  draw_frame_table(stats);

  ImGui::SeparatorText("CPU");
  draw_profiler_flame_graph();

  ImGui::SeparatorText("Memory by Subsystem");
  draw_memory_table();

  ImGui::End();
}

void draw_in_game_stats_overlay(const core::EngineStats &stats) noexcept {
  constexpr ImGuiWindowFlags kOverlayFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;

  // The stats are the Game view's frame, so they anchor inside its image
  // when it is shown, else inside the Scene view's; the old fixed position
  // sat on top of the docked Entities panel.
  ImVec2 overlayPos(editor_px(12.0F), editor_px(44.0F));
  const EditorSession &session = editor_session();
  if (session.gameViewShown && (session.gameViewScreenSize.x > 0.0F) &&
      (session.gameViewScreenSize.y > 0.0F)) {
    overlayPos = ImVec2(session.gameViewScreenPos.x + editor_px(12.0F),
                        session.gameViewScreenPos.y + editor_px(12.0F));
  } else if ((session.sceneViewportScreenSize.x > 0.0F) &&
             (session.sceneViewportScreenSize.y > 0.0F)) {
    overlayPos = ImVec2(session.sceneViewportScreenPos.x + editor_px(12.0F),
                        session.sceneViewportScreenPos.y + editor_px(12.0F));
  }
  ImGui::SetNextWindowBgAlpha(0.40F);
  ImGui::SetNextWindowPos(overlayPos, ImGuiCond_Always);
  if (!ImGui::Begin("##InGameStatsOverlay", nullptr, kOverlayFlags)) {
    ImGui::End();
    return;
  }

  ImGui::Text("FPS %.1f | Frame %.2f ms", static_cast<double>(stats.fps),
              static_cast<double>(stats.frameTimeMs));
  ImGui::Text("Draw %u | Tris %llu", stats.drawCalls,
              static_cast<unsigned long long>(stats.triCount));
  ImGui::Text("Entities %zu | Mem %.1f MB", stats.entityCount,
              static_cast<double>(stats.memoryUsedMb));

  ImGui::End();
}


} // namespace engine::editor
