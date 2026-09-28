// Verifies the Profiler's frame history: empty, it reads zeros; one frame
// is its own current, average and maximum; full, it keeps exactly
// kFrameHistoryCapacity frames oldest first; one past full drops the
// oldest and keeps the newest, so a spike stays graphed until it ages out.

#include "editor_frame_history.h"

#include "../test_harness.h"

namespace {

using engine::editor::FrameSeries;
using engine::editor::kFrameHistoryCapacity;

engine::core::EngineStats frame(float ms) noexcept {
  engine::core::EngineStats stats{};
  stats.frameTimeMs = ms;
  stats.drawCalls = static_cast<std::uint32_t>(ms);
  return stats;
}

} // namespace

int main() {
  engine::tests::TestContext t;
  using namespace engine::editor;

  frame_history_clear();
  const FrameSeriesSummary empty = frame_history_summary(FrameSeries::FrameMs);
  t.check((frame_history_count() == 0U) && (empty.current == 0.0F) &&
              (empty.average == 0.0F) && (empty.maximum == 0.0F) &&
              (frame_history_value(FrameSeries::FrameMs, 0U) == 0.0F),
          "an empty history reads zeros");

  frame_history_push(frame(16.0F));
  const FrameSeriesSummary one = frame_history_summary(FrameSeries::FrameMs);
  t.check((frame_history_count() == 1U) && (one.current == 16.0F) &&
              (one.average == 16.0F) && (one.maximum == 16.0F),
          "one frame is its own current, average and maximum");

  frame_history_clear();
  for (std::size_t i = 0U; i < kFrameHistoryCapacity; ++i) {
    frame_history_push(frame(static_cast<float>(i)));
  }
  const FrameSeriesSummary full = frame_history_summary(FrameSeries::FrameMs);
  const float last = static_cast<float>(kFrameHistoryCapacity - 1U);
  t.check(frame_history_count() == kFrameHistoryCapacity,
          "a full history holds exactly its capacity");
  t.check((frame_history_value(FrameSeries::FrameMs, 0U) == 0.0F) &&
              (full.current == last) && (full.maximum == last),
          "frames are kept oldest first");
  // 0..299 sums to 44850, exactly representable, so the mean is exact.
  t.check(full.average == (last / 2.0F), "the average spans every frame");
  t.check(frame_history_value(FrameSeries::DrawCalls, 5U) == 5.0F,
          "each series reads its own field");

  frame_history_push(frame(1000.0F));
  const FrameSeriesSummary past = frame_history_summary(FrameSeries::FrameMs);
  t.check(frame_history_count() == kFrameHistoryCapacity,
          "one past full keeps the capacity");
  t.check((frame_history_value(FrameSeries::FrameMs, 0U) == 1.0F) &&
              (past.current == 1000.0F) && (past.maximum == 1000.0F),
          "the oldest frame is dropped and the spike kept");
  t.check(frame_history_value(FrameSeries::FrameMs, kFrameHistoryCapacity) ==
              0.0F,
          "an index past the end reads zero");
  return t.finish("editor_frame_history");
}
