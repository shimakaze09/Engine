// Implements the editor stats panel, profiler flame graph, and in-game overlay.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_diagnostics.h"

#include "editor_commands.h"
#include "editor_session.h"

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

namespace engine::editor {

namespace {

void draw_profiler_flame_graph() noexcept {
  std::array<core::ProfileEntry, 256U> entries{};
  const std::size_t count =
      core::profiler_get_entries(entries.data(), entries.size());
  if (count == 0U) {
    ImGui::TextUnformatted("Profiler: no samples");
    return;
  }

  const float frameMs = core::profiler_frame_time_ms();
  const float graphMs = (frameMs > 0.001F) ? frameMs : 0.001F;
  const float graphWidth = ImGui::GetContentRegionAvail().x;
  const float barHeight = 16.0F;
  const float barSpacing = 4.0F;

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
    drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color, 2.0F);

    char label[96] = {};
    const char *name = (entry.name != nullptr) ? entry.name : "<unnamed>";
    std::snprintf(label, sizeof(label), "%s %.2fms", name,
                  static_cast<double>(entry.durationMs));
    drawList->AddText(ImVec2(x0 + 2.0F, y0 + 1.0F), IM_COL32(0, 0, 0, 255),
                      label);
  }

  const float graphHeight =
      static_cast<float>(maxDepth + 1U) * (barHeight + barSpacing);
  ImGui::Dummy(ImVec2(graphWidth, graphHeight));
}


} // namespace

void draw_stats_panel(const core::EngineStats &stats) noexcept {
  if (!ImGui::Begin("Stats")) {
    ImGui::End();
    return;
  }

  ImGui::Text("FPS: %.1f", static_cast<double>(stats.fps));
  ImGui::Text("Frame: %.3f ms", static_cast<double>(stats.frameTimeMs));
  ImGui::Text("Draw Calls: %u", stats.drawCalls);
  ImGui::Text("Triangles: %llu",
              static_cast<unsigned long long>(stats.triCount));
  ImGui::Text("Entities: %zu", stats.entityCount);
  ImGui::Text("Memory: %.2f MB", static_cast<double>(stats.memoryUsedMb));
  ImGui::Text("GPU Scene: %.3f ms", static_cast<double>(stats.gpuSceneMs));
  ImGui::Text("GPU Tonemap: %.3f ms", static_cast<double>(stats.gpuTonemapMs));
  ImGui::Text("Job Utilization: %.2f%%",
              static_cast<double>(stats.jobUtilizationPct));

  ImGui::Separator();
  ImGui::TextUnformatted("CPU Flame Graph");
  draw_profiler_flame_graph();

  ImGui::Separator();
  ImGui::TextUnformatted("Memory by Subsystem");
  {
    std::array<core::MemTagSnapshot, core::kMemTagCount> snaps{};
    const std::size_t count =
        core::mem_tracker_snapshot(snaps.data(), snaps.size());
    float maxBytes = 1.0F; // avoid division by zero
    for (std::size_t i = 0U; i < count; ++i) {
      const float bytes = static_cast<float>(
          snaps[i].currentBytes > 0 ? snaps[i].currentBytes : 0);
      if (bytes > maxBytes) {
        maxBytes = bytes;
      }
    }
    for (std::size_t i = 0U; i < count; ++i) {
      const float bytes = static_cast<float>(
          snaps[i].currentBytes > 0 ? snaps[i].currentBytes : 0);
      const float mb = bytes / (1024.0F * 1024.0F);
      char label[64]{};
      std::snprintf(label, sizeof(label), "%.2f MB", static_cast<double>(mb));
      ImGui::Text("%s", core::mem_tag_name(snaps[i].tag));
      ImGui::SameLine(100.0F);
      ImGui::ProgressBar(bytes / maxBytes, ImVec2(-1.0F, 0.0F), label);
    }
  }

  ImGui::End();
}


void draw_in_game_stats_overlay(const core::EngineStats &stats) noexcept {
  constexpr ImGuiWindowFlags kOverlayFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;

  ImGui::SetNextWindowBgAlpha(0.40F);
  ImGui::SetNextWindowPos(ImVec2(12.0F, 44.0F), ImGuiCond_Always);
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
