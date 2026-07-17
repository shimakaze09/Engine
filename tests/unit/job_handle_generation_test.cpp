// Verifies job handles stay valid after the graph generation counter passes
// the width that survives handle encoding (regression test for the truncation
// bug that invalidated every handle after ~2^19 frame graphs).

#include "engine/core/job_system.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

// Handles encode (generation << 13) | index, leaving 19 generation bits.
// Cycle enough graphs to push the internal counter well past that boundary.
constexpr std::uint32_t kEncodableGenerations = 1U << (32U - 13U);
constexpr std::uint32_t kGraphCycles = kEncodableGenerations + 64U;

std::atomic<std::uint32_t> g_executions{0U};

/// Counts executions so the test can prove the submitted job actually ran.
void counting_job(void *) noexcept {
  g_executions.fetch_add(1U, std::memory_order_relaxed);
}

/// Submits one job, validates its handle, waits, and checks completion.
/// Returns 0 on success or a nonzero failure code.
int run_one_graph(bool checkExecution) noexcept {
  if (!engine::core::begin_frame_graph()) {
    return 10;
  }

  engine::core::Job job{};
  job.function = &counting_job;
  const engine::core::JobHandle handle = engine::core::submit(job);

  if (!engine::core::is_valid_handle(handle)) {
    return 11;
  }

  engine::core::wait(handle);

  if (checkExecution && !engine::core::is_completed(handle)) {
    return 12;
  }

  if (!engine::core::end_frame_graph()) {
    return 13;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_job_system(2U)) {
    return 1;
  }

  // Spin the generation counter past the encodable width. Empty graphs are
  // enough: begin/end each advance the counter.
  for (std::uint32_t i = 0U; i < kGraphCycles; ++i) {
    if (!engine::core::begin_frame_graph()) {
      std::fprintf(stderr, "begin_frame_graph failed at cycle %u\n", i);
      engine::core::shutdown_job_system();
      return 2;
    }
    if (!engine::core::end_frame_graph()) {
      std::fprintf(stderr, "end_frame_graph failed at cycle %u\n", i);
      engine::core::shutdown_job_system();
      return 3;
    }
  }

  // Handles minted after the wrap must still validate, dispatch, and complete.
  const std::uint32_t executionsBefore =
      g_executions.load(std::memory_order_relaxed);
  const int result = run_one_graph(true);
  if (result != 0) {
    std::fprintf(stderr, "post-wrap graph failed with code %d\n", result);
    engine::core::shutdown_job_system();
    return result;
  }

  const std::uint32_t executionsAfter =
      g_executions.load(std::memory_order_relaxed);

  engine::core::shutdown_job_system();

  if (executionsAfter != (executionsBefore + 1U)) {
    std::fprintf(stderr, "job did not execute after generation wrap\n");
    return 4;
  }

  return 0;
}
