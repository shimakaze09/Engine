// Pins the ready-queue capacity invariant (issue #71): a frame graph sized
// at the job-capacity boundary must execute every job and end cleanly, both
// when dispatch readies every node at once (zero workers, so nothing drains
// the queue while it fills to exactly capacity) and when one fan-in job's
// completion readies the maximum burst of dependents. If the ready queue
// could overflow, the dropped job would never run and end_frame_graph would
// report failure (or the graph would hang and the test would time out).

#include "engine/core/job_system.h"

#include <atomic>
#include <cstddef>
#include <cstdio>

namespace {

// Mirrors the internal kMaxJobs node cap in core/src/job_system.cpp; the
// 8193rd submit below fails iff this matches the production constant.
constexpr std::size_t kJobCapacity = 8192U;

std::atomic<std::size_t> g_executedCount{0U};

/// Counting job body shared by every node in both scenarios.
void counting_job(void *) noexcept {
  g_executedCount.fetch_add(1U, std::memory_order_relaxed);
}

/// Fills one graph with kJobCapacity independent jobs on a zero-worker
/// system: dispatch pushes every node into the ready queue with no
/// concurrent pops, holding it at exactly capacity, and the waiting main
/// thread must then drain and execute all of them.
int run_dispatch_boundary_round() noexcept {
  g_executedCount.store(0U, std::memory_order_relaxed);

  if (!engine::core::begin_frame_graph()) {
    return 10;
  }

  engine::core::Job job{};
  job.function = &counting_job;

  engine::core::JobHandle lastHandle{};
  for (std::size_t i = 0U; i < kJobCapacity; ++i) {
    lastHandle = engine::core::submit(job);
    if (!engine::core::is_valid_handle(lastHandle)) {
      return 11;
    }
  }

  const engine::core::JobHandle overflowHandle = engine::core::submit(job);
  if (engine::core::is_valid_handle(overflowHandle)) {
    return 12;
  }

  engine::core::wait(lastHandle);
  if (!engine::core::end_frame_graph()) {
    return 13;
  }

  if (g_executedCount.load(std::memory_order_relaxed) != kJobCapacity) {
    return 14;
  }
  return 0;
}

/// Builds one root job fanning out to every remaining node: the root's
/// completion readies kJobCapacity - 1 dependents in a single burst, the
/// widest fan-in push the node cap permits.
int run_fan_in_boundary_round() noexcept {
  g_executedCount.store(0U, std::memory_order_relaxed);

  if (!engine::core::begin_frame_graph()) {
    return 20;
  }

  engine::core::Job job{};
  job.function = &counting_job;

  const engine::core::JobHandle rootHandle = engine::core::submit(job);
  if (!engine::core::is_valid_handle(rootHandle)) {
    return 21;
  }

  engine::core::JobHandle lastHandle{};
  for (std::size_t i = 1U; i < kJobCapacity; ++i) {
    lastHandle = engine::core::submit(job);
    if (!engine::core::is_valid_handle(lastHandle)) {
      return 22;
    }
    if (!engine::core::add_dependency(rootHandle, lastHandle)) {
      return 23;
    }
  }

  engine::core::wait(lastHandle);
  if (!engine::core::end_frame_graph()) {
    return 24;
  }

  if (g_executedCount.load(std::memory_order_relaxed) != kJobCapacity) {
    return 25;
  }
  return 0;
}

/// Runs one scenario under a freshly initialized job system with the given
/// worker count, then shuts the system down again.
int run_with_workers(std::uint32_t workerCount, int (*round)() noexcept,
                     const char *label) noexcept {
  if (!engine::core::initialize_job_system(workerCount)) {
    std::fprintf(stderr,
                 "job_ready_queue_capacity_test: init failed (%s)\n", label);
    return 1;
  }

  const int result = round();
  engine::core::shutdown_job_system();
  if (result != 0) {
    std::fprintf(stderr, "job_ready_queue_capacity_test failed: %d (%s)\n",
                 result, label);
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result =
      run_with_workers(0U, &run_dispatch_boundary_round, "dispatch/0 workers");
  if (result == 0) {
    result = run_with_workers(3U, &run_dispatch_boundary_round,
                              "dispatch/3 workers");
  }
  if (result == 0) {
    result =
        run_with_workers(0U, &run_fan_in_boundary_round, "fan-in/0 workers");
  }
  if (result == 0) {
    result =
        run_with_workers(3U, &run_fan_in_boundary_round, "fan-in/3 workers");
  }

  if (result == 0) {
    std::printf("job_ready_queue_capacity_test: all tests passed\n");
  }
  return result;
}
