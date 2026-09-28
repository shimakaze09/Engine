// Declares the editor's two stats surfaces: the Profiler window (frame
// numbers, CPU flame graph, memory by subsystem) and the lightweight
// overlay over the Game or Scene view. Both are off until asked for.

#pragma once

#include "engine/core/engine_stats.h"

namespace engine::editor {

/// The cvar that shows the stats overlay (the toolbar's Stats toggle,
/// saved as ShowStats=).
inline constexpr const char *kShowStatsCvar = "r_showStats";
/// The cvar that shows the Profiler window (Window > Profiler).
inline constexpr const char *kShowProfilerCvar = "editor.show_profiler";

/// Registers r_showStats and editor.show_profiler, both off by default.
/// Called before the layout is read, since its preferences set them.
void register_stats_cvars() noexcept;
/// Draws the Profiler window while editor.show_profiler is set; its close
/// button clears the cvar. It opens as a tab beside the Console.
void draw_profiler_panel(const core::EngineStats &stats) noexcept;
/// Draws the minimal stats overlay; the caller draws it only while
/// r_showStats (the toolbar's Stats toggle) is set.
void draw_in_game_stats_overlay(const core::EngineStats &stats) noexcept;

} // namespace engine::editor
