// Declares the editor stats panel, profiler flame graph, and in-game overlay.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include "engine/core/engine_stats.h"

namespace engine::editor {

/// Draws the engine stats panel (memory bars, GPU timings, flame graph).
void draw_stats_panel(const core::EngineStats &stats) noexcept;
/// Draws the minimal in-game stats overlay (r_showStats).
void draw_in_game_stats_overlay(const core::EngineStats &stats) noexcept;

} // namespace engine::editor
