// Verifies the GPU profiler query ring against a fake render device with
// scripted result availability (audit M-06): a write slot whose queries
// are still unresolved is skipped with back-pressure (droppedFrames) and
// never re-issued, an end mark without a begin mark issues no timestamp,
// out-of-range pass ids never alias the Scene slot, delayed results
// still resolve to exact durations once available, and a pass that stops
// running publishes zero once the frame without it resolves while a pass
// whose results are merely late keeps its last measurement (#494).

#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/render_device.h"

#include <cstdint>
#include <cstdio>

namespace engine::renderer {

namespace {

std::uint32_t g_nextQueryId = 1U;
std::uint64_t g_timestampCalls = 0U;
std::uint64_t g_nextTimestampNs = 0U;
std::uint64_t g_queryResults[512]{};
bool g_resultsAvailable = true;

DeviceQueryHandle fake_create_query() noexcept {
  return DeviceQueryHandle{g_nextQueryId++};
}
void fake_destroy_query(DeviceQueryHandle) noexcept {}
void fake_write_timestamp(DeviceQueryHandle query) noexcept {
  ++g_timestampCalls;
  if (query.value < 512U) {
    g_queryResults[query.value] = g_nextTimestampNs;
  }
}
bool fake_timestamp_ready(DeviceQueryHandle) noexcept {
  return g_resultsAvailable;
}
std::uint64_t fake_timestamp_value(DeviceQueryHandle query) noexcept {
  return (query.value < 512U) ? g_queryResults[query.value] : 0U;
}

RenderDevice g_device{};

} // namespace

/// Link seam: the profiler TU resolves its device through this override.
const RenderDevice *render_device() noexcept {
  g_device.caps.timestampQueries = true;
  g_device.create_timestamp_query = &fake_create_query;
  g_device.destroy_timestamp_query = &fake_destroy_query;
  g_device.write_timestamp = &fake_write_timestamp;
  g_device.timestamp_ready = &fake_timestamp_ready;
  g_device.timestamp_value = &fake_timestamp_value;
  return &g_device;
}

} // namespace engine::renderer

namespace {

using namespace engine::renderer;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Runs one profiled frame: begin frame, then a Scene begin/end pair with
/// the given begin/end GPU timestamps.
void run_frame(std::uint64_t beginNs, std::uint64_t endNs) noexcept {
  gpu_profiler_begin_frame();
  g_nextTimestampNs = beginNs;
  gpu_profiler_begin_pass(GpuPassId::Scene);
  g_nextTimestampNs = endNs;
  gpu_profiler_end_pass(GpuPassId::Scene);
}

/// EXPECTATION (audit M-06): while GPU results are unavailable the ring
/// stops issuing timestamps into unresolved slots and counts the dropped
/// frames; once results land, the delayed frame resolves exactly.
void test_ring_backpressure_and_delayed_resolve() noexcept {
  CHECK(initialize_gpu_profiler(), "profiler initializes");

  run_frame(1000000U, 3000000U);
  run_frame(2000000U, 6000000U);
  CHECK(g_timestampCalls == 4U, "two clean frames issue four timestamps");

  g_resultsAvailable = false;
  const std::uint64_t callsBefore = g_timestampCalls;
  run_frame(5000000U, 9000000U);
  CHECK(g_timestampCalls == callsBefore,
        "unresolved write slot issues no timestamps");
  CHECK(gpu_profiler_debug_stats().droppedFrames == 1U,
        "dropped frame is counted");

  run_frame(5000000U, 9000000U);
  CHECK(g_timestampCalls == callsBefore,
        "back-pressure holds while results stay unavailable");
  CHECK(gpu_profiler_debug_stats().droppedFrames == 2U,
        "second dropped frame is counted");

  g_resultsAvailable = true;
  gpu_profiler_begin_frame();
  gpu_profiler_begin_frame();
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 4.0F,
        "delayed results resolve to the exact duration");

  shutdown_gpu_profiler();
}

/// EXPECTATION (audit M-06): an end mark without a begin mark must not
/// submit a range pairing a fresh end timestamp with a stale begin.
void test_end_without_begin_is_ignored() noexcept {
  g_resultsAvailable = true;
  CHECK(initialize_gpu_profiler(), "profiler initializes");

  gpu_profiler_begin_frame();
  const std::uint64_t callsBefore = g_timestampCalls;
  gpu_profiler_end_pass(GpuPassId::Scene);
  CHECK(g_timestampCalls == callsBefore,
        "unpaired end mark issues no timestamp");

  shutdown_gpu_profiler();
}

/// EXPECTATION (audit M-06): an out-of-range pass id is ignored instead of
/// silently folding onto the Scene slot.
void test_out_of_range_pass_does_not_alias_scene() noexcept {
  g_resultsAvailable = true;
  CHECK(initialize_gpu_profiler(), "profiler initializes");

  run_frame(1000000U, 2000000U);
  gpu_profiler_begin_frame();
  gpu_profiler_begin_frame();
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 1.0F,
        "scene duration resolved");

  const GpuPassId bogus = static_cast<GpuPassId>(200U);
  const std::uint64_t callsBefore = g_timestampCalls;
  gpu_profiler_begin_pass(bogus);
  gpu_profiler_end_pass(bogus);
  CHECK(g_timestampCalls == callsBefore, "bogus pass issues no timestamps");
  CHECK(gpu_profiler_pass_ms(bogus) == 0.0F, "bogus pass reads zero");
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 1.0F,
        "scene slot untouched by bogus pass");

  shutdown_gpu_profiler();
}

/// Runs one profiled frame with a Scene pass and, optionally, a Tonemap
/// pass; each pass takes the given whole number of milliseconds.
void run_frame_with_passes(std::uint64_t sceneMs, bool withTonemap,
                           std::uint64_t tonemapMs) noexcept {
  gpu_profiler_begin_frame();
  g_nextTimestampNs = 0U;
  gpu_profiler_begin_pass(GpuPassId::Scene);
  g_nextTimestampNs = sceneMs * 1000000U;
  gpu_profiler_end_pass(GpuPassId::Scene);
  if (withTonemap) {
    g_nextTimestampNs = 10000000U;
    gpu_profiler_begin_pass(GpuPassId::Tonemap);
    g_nextTimestampNs = 10000000U + tonemapMs * 1000000U;
    gpu_profiler_end_pass(GpuPassId::Tonemap);
  }
}

/// EXPECTATION (#494): the published duration belongs to the most recently
/// resolved frame. A pass that issued no queries in that frame reads zero
/// instead of holding the value an earlier frame measured, a pass that
/// kept running keeps publishing its fresh value, and a pass that resumes
/// publishes its new measurement. The ring resolves a frame two frames
/// after it was written, so each expectation is read two frames on.
void test_pass_that_stops_running_reads_zero() noexcept {
  g_resultsAvailable = true;
  CHECK(initialize_gpu_profiler(), "profiler initializes");

  run_frame_with_passes(1U, true, 2U);  // frame 1: both passes
  run_frame_with_passes(3U, false, 0U); // frame 2: tonemap stops
  run_frame_with_passes(3U, false, 0U); // frame 3: resolves frame 1
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 1.0F,
        "first frame's scene duration resolved");
  CHECK(gpu_profiler_pass_ms(GpuPassId::Tonemap) == 2.0F,
        "first frame's tonemap duration resolved");

  run_frame_with_passes(3U, false, 0U); // frame 4: resolves frame 2
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 3.0F,
        "the continuously running pass publishes the newer frame's value");
  CHECK(gpu_profiler_pass_ms(GpuPassId::Tonemap) == 0.0F,
        "a pass absent from the resolved frame reads zero, not its last "
        "measurement");

  run_frame_with_passes(4U, true, 5U); // frame 5: tonemap resumes
  run_frame_with_passes(4U, true, 5U); // frame 6: resolves frame 4
  CHECK(gpu_profiler_pass_ms(GpuPassId::Tonemap) == 0.0F,
        "the frame before the resume still reads zero");
  run_frame_with_passes(4U, true, 5U); // frame 7: resolves frame 5
  CHECK(gpu_profiler_pass_ms(GpuPassId::Tonemap) == 5.0F,
        "a pass that resumes publishes its new measurement");
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 4.0F,
        "the running pass publishes the resumed frame's value");

  shutdown_gpu_profiler();
}

/// EXPECTATION (#494 boundary): absence and lateness are different. A pass
/// whose queries were submitted but whose GPU results have not landed keeps
/// its last resolved value; only a frame that never issued the pass zeroes
/// it. Once the late results land, the frame resolves to its own values.
void test_late_results_keep_last_value() noexcept {
  g_resultsAvailable = true;
  CHECK(initialize_gpu_profiler(), "profiler initializes");

  run_frame_with_passes(1U, true, 2U); // frame 1
  run_frame_with_passes(6U, true, 7U); // frame 2: the frame that will be late
  run_frame_with_passes(1U, true, 2U); // frame 3: resolves frame 1
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 1.0F, "scene resolved");
  CHECK(gpu_profiler_pass_ms(GpuPassId::Tonemap) == 2.0F, "tonemap resolved");

  // Frame 2's results are not ready when its slot comes up: both published
  // values stay at frame 1's measurements instead of dropping to zero.
  g_resultsAvailable = false;
  gpu_profiler_begin_frame(); // frame 4: frame 2 pending, write slot blocked
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 1.0F,
        "late scene results keep the last resolved value");
  CHECK(gpu_profiler_pass_ms(GpuPassId::Tonemap) == 2.0F,
        "late tonemap results keep the last resolved value");
  CHECK(gpu_profiler_debug_stats().droppedFrames == 1U,
        "the blocked write slot is counted as a dropped frame");

  g_resultsAvailable = true;
  gpu_profiler_begin_frame(); // frame 5: resolves frame 3 (1 ms / 2 ms)
  gpu_profiler_begin_frame(); // frame 6: resolves the late frame 2
  CHECK(gpu_profiler_pass_ms(GpuPassId::Scene) == 6.0F,
        "delayed scene results resolve to their frame's value");
  CHECK(gpu_profiler_pass_ms(GpuPassId::Tonemap) == 7.0F,
        "delayed tonemap results resolve to their frame's value");

  shutdown_gpu_profiler();
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== GPU Profiler Ring Unit Tests ===\n");

  test_ring_backpressure_and_delayed_resolve();
  test_end_without_begin_is_ignored();
  test_out_of_range_pass_does_not_alias_scene();
  test_pass_that_stops_running_reads_zero();
  test_late_results_keep_last_value();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
