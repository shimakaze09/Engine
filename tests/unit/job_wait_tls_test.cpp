// Verifies that engine::core::wait never mutates the calling thread's
// thread-local worker index (audit M-12): a worker that waits inside a job
// must keep its own current_thread_index so job stats stay attributed to the
// executing thread and later helping runs on the right identity.

#include "engine/core/job_system.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

constexpr std::uint32_t kJobsPerGraph = 64U;
constexpr std::uint32_t kMaxAttempts = 20U;

/// Thread-index observation captured by one job around a production wait().
struct WaitObservation final {
  std::uint32_t before = 0U;
  std::uint32_t after = 0U;
  bool executed = false;
};

WaitObservation g_observations[kJobsPerGraph];

/// Job body: records current_thread_index before and after calling the
/// production wait() entry point (with an invalid handle so the call returns
/// immediately instead of deadlocking on its own pending job).
void observing_job(void *data) noexcept {
  auto *slot = static_cast<WaitObservation *>(data);
  slot->before = engine::core::current_thread_index();
  engine::core::wait(engine::core::JobHandle{});
  slot->after = engine::core::current_thread_index();
  slot->executed = true;

  // Small busy spin so jobs overlap and workers pick up a share of the graph.
  volatile std::uint32_t sink = 0U;
  for (std::uint32_t i = 0U; i < 20000U; ++i) {
    sink = sink + i;
  }
  static_cast<void>(sink);
}

/// Runs one graph of observing jobs; returns 0 on contract failure paths,
/// otherwise the number of jobs that executed on a nonzero thread index.
int run_observed_graph() noexcept {
  for (auto &slot : g_observations) {
    slot = WaitObservation{};
  }

  if (!engine::core::begin_frame_graph()) {
    return -1;
  }

  engine::core::JobHandle lastHandle{};
  for (std::uint32_t i = 0U; i < kJobsPerGraph; ++i) {
    engine::core::Job job{};
    job.function = &observing_job;
    job.data = &g_observations[i];
    lastHandle = engine::core::submit(job);
    if (!engine::core::is_valid_handle(lastHandle)) {
      return -2;
    }
  }

  engine::core::wait(lastHandle);
  if (!engine::core::end_frame_graph()) {
    return -3;
  }

  int workerExecutions = 0;
  for (const auto &slot : g_observations) {
    if (!slot.executed) {
      return -4;
    }
    if (slot.after != slot.before) {
      std::fprintf(stderr,
                   "wait() changed current_thread_index: %u -> %u\n",
                   slot.before, slot.after);
      return -5;
    }
    if (slot.before != 0U) {
      ++workerExecutions;
    }
  }
  return workerExecutions;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_job_system(4U)) {
    std::fprintf(stderr, "job system init failed\n");
    return 1;
  }

  // The main thread's index must stay 0 across a full wait-driven graph.
  if (engine::core::current_thread_index() != 0U) {
    std::fprintf(stderr, "main thread index not 0 at start\n");
    return 2;
  }

  bool workerCovered = false;
  for (std::uint32_t attempt = 0U; attempt < kMaxAttempts; ++attempt) {
    const int result = run_observed_graph();
    if (result < 0) {
      std::fprintf(stderr, "graph run failed (%d)\n", result);
      engine::core::shutdown_job_system();
      return 3;
    }
    if (result > 0) {
      workerCovered = true;
      break;
    }
  }

  if (!workerCovered) {
    std::fprintf(stderr, "no job ever executed on a worker thread\n");
    engine::core::shutdown_job_system();
    return 4;
  }

  if (engine::core::current_thread_index() != 0U) {
    std::fprintf(stderr, "main thread index not 0 after graphs\n");
    engine::core::shutdown_job_system();
    return 5;
  }

  engine::core::shutdown_job_system();
  std::printf("job_wait_tls_test passed\n");
  return 0;
}
