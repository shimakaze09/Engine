// Declares the editor's frame history: the last kFrameHistoryCapacity
// frames' performance numbers, sampled from core::EngineStats once per
// editor frame, which the Profiler draws as graphs. A fixed ring, as
// Unity's Profiler keeps its last 300 frames: a spike stays visible for a
// few seconds instead of scrolling past as a log line.
// Panel-draw-code exempt: every symbol here is testable without ImGui.

#pragma once

#include "engine/core/engine_stats.h"

#include <cstddef>

namespace engine::editor {

/// Frames the history keeps; the oldest is dropped past this.
inline constexpr std::size_t kFrameHistoryCapacity = 300U;

/// The per-frame numbers the Profiler graphs.
enum class FrameSeries : unsigned char {
  FrameMs,
  DrawCalls,
  MemoryMb,
  JobUtilizationPct,
};

/// A series' readouts over the kept frames.
struct FrameSeriesSummary final {
  float current = 0.0F; ///< The newest frame's value.
  float average = 0.0F;
  float maximum = 0.0F;
};

/// Appends one frame's numbers, dropping the oldest once full.
void frame_history_push(const core::EngineStats &stats) noexcept;
/// Frames kept, at most kFrameHistoryCapacity.
std::size_t frame_history_count() noexcept;
/// The `index`th kept frame's value of `series`, oldest first; 0 past the
/// end.
float frame_history_value(FrameSeries series, std::size_t index) noexcept;
/// Current, average and maximum of `series`; all zero with no frames.
FrameSeriesSummary frame_history_summary(FrameSeries series) noexcept;
/// Forgets every kept frame.
void frame_history_clear() noexcept;

} // namespace engine::editor
