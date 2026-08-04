// Verifies the GPU profiler query ring against a fake render device with
// scripted result availability (audit M-06): a write slot whose queries
// are still unresolved is skipped with back-pressure (droppedFrames) and
// never re-issued, an end mark without a begin mark issues no timestamp,
// out-of-range pass ids never alias the Scene slot, and delayed results
// still resolve to exact durations once available.

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

std::uint32_t fake_create_query() noexcept { return g_nextQueryId++; }
void fake_destroy_query(std::uint32_t) noexcept {}
void fake_query_counter_timestamp(std::uint32_t query) noexcept {
  ++g_timestampCalls;
  if (query < 512U) {
    g_queryResults[query] = g_nextTimestampNs;
  }
}
bool fake_query_result_available(std::uint32_t) noexcept {
  return g_resultsAvailable;
}
std::uint64_t fake_query_result_u64(std::uint32_t query) noexcept {
  return (query < 512U) ? g_queryResults[query] : 0U;
}

RenderDevice g_device{};

} // namespace

/// Link seam: the profiler TU resolves its device through this override.
const RenderDevice *render_device() noexcept {
  g_device.create_query = &fake_create_query;
  g_device.destroy_query = &fake_destroy_query;
  g_device.query_counter_timestamp = &fake_query_counter_timestamp;
  g_device.query_result_available = &fake_query_result_available;
  g_device.query_result_u64 = &fake_query_result_u64;
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

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== GPU Profiler Ring Unit Tests ===\n");

  test_ring_backpressure_and_delayed_resolve();
  test_end_without_begin_is_ignored();
  test_out_of_range_pass_does_not_alias_scene();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
