// Implements the editor's frame history on a fixed ring of samples.

#include "editor_frame_history.h"

#include "engine/core/fixed_ring.h"

namespace engine::editor {

namespace {

/// One frame's graphed numbers.
struct FrameSample final {
  float frameMs = 0.0F;
  float drawCalls = 0.0F;
  float memoryMb = 0.0F;
  float jobUtilizationPct = 0.0F;
};

core::FixedRing<FrameSample, kFrameHistoryCapacity> g_history{};

float sample_value(const FrameSample &sample, FrameSeries series) noexcept {
  switch (series) {
  case FrameSeries::FrameMs:
    return sample.frameMs;
  case FrameSeries::DrawCalls:
    return sample.drawCalls;
  case FrameSeries::MemoryMb:
    return sample.memoryMb;
  case FrameSeries::JobUtilizationPct:
    return sample.jobUtilizationPct;
  }
  return 0.0F;
}

} // namespace

void frame_history_push(const core::EngineStats &stats) noexcept {
  FrameSample sample{};
  sample.frameMs = stats.frameTimeMs;
  sample.drawCalls = static_cast<float>(stats.drawCalls);
  sample.memoryMb = stats.memoryUsedMb;
  sample.jobUtilizationPct = stats.jobUtilizationPct;
  static_cast<void>(g_history.push_overwrite(sample));
}

std::size_t frame_history_count() noexcept { return g_history.size(); }

float frame_history_value(FrameSeries series, std::size_t index) noexcept {
  const FrameSample *sample = g_history.at(index);
  return (sample != nullptr) ? sample_value(*sample, series) : 0.0F;
}

FrameSeriesSummary frame_history_summary(FrameSeries series) noexcept {
  FrameSeriesSummary summary{};
  const std::size_t count = g_history.size();
  if (count == 0U) {
    return summary;
  }
  double total = 0.0;
  for (std::size_t i = 0U; i < count; ++i) {
    const float value = frame_history_value(series, i);
    total += static_cast<double>(value);
    summary.maximum = (value > summary.maximum) ? value : summary.maximum;
  }
  summary.current = frame_history_value(series, count - 1U);
  summary.average = static_cast<float>(total / static_cast<double>(count));
  return summary;
}

void frame_history_clear() noexcept { g_history.clear(); }

} // namespace engine::editor
