// Verifies the handle-specific wait contract (issue #109): wait(handle)
// returns when that job completes even while unrelated jobs stay pending,
// and wait_all drains the whole graph. Zero-worker rounds are fully
// deterministic: the main thread is the only executor and the ready queue
// is FIFO, so submission order is execution order while helping.

#include "engine/core/job_system.h"

#include <atomic>
#include <cstdio>

namespace {

std::atomic<std::uint32_t> g_executedCount{0U};
std::atomic<bool> g_signal{false};
std::atomic<bool> g_signalObserved{false};

/// Job body: counts executions.
void counting_job(void *) noexcept {
  g_executedCount.fetch_add(1U, std::memory_order_relaxed);
}

/// Job body: records whether the main-thread signal was already set.
void signal_reading_job(void *) noexcept {
  g_signalObserved.store(g_signal.load(std::memory_order_acquire),
                         std::memory_order_release);
  g_executedCount.fetch_add(1U, std::memory_order_relaxed);
}

/// Issue #109 core regression: with zero workers, wait(A) must execute and
/// return at A, leaving the unrelated later-submitted B pending for
/// wait_all instead of draining the whole graph.
int run_handle_scope_round() noexcept {
  g_executedCount.store(0U, std::memory_order_relaxed);

  if (!engine::core::begin_frame_graph()) {
    return 10;
  }

  engine::core::Job job{};
  job.function = &counting_job;
  const engine::core::JobHandle handleA = engine::core::submit(job);
  const engine::core::JobHandle handleB = engine::core::submit(job);
  if (!engine::core::is_valid_handle(handleA) ||
      !engine::core::is_valid_handle(handleB)) {
    return 11;
  }

  engine::core::wait(handleA);
  if (!engine::core::is_completed(handleA)) {
    return 12;
  }
  if (engine::core::is_completed(handleB)) {
    return 13;
  }
  if (g_executedCount.load(std::memory_order_relaxed) != 1U) {
    return 14;
  }

  // Waiting an already-completed handle must return without touching B.
  engine::core::wait(handleA);
  if (engine::core::is_completed(handleB)) {
    return 15;
  }

  engine::core::wait_all();
  if (!engine::core::is_completed(handleB) ||
      (g_executedCount.load(std::memory_order_relaxed) != 2U)) {
    return 16;
  }

  if (!engine::core::end_frame_graph()) {
    return 17;
  }
  return 0;
}

/// Issue #109 deadlock scenario: the unrelated job depends on main-thread
/// work performed only after wait(requested) returns.
int run_deferred_signal_round() noexcept {
  g_executedCount.store(0U, std::memory_order_relaxed);
  g_signal.store(false, std::memory_order_release);
  g_signalObserved.store(false, std::memory_order_release);

  if (!engine::core::begin_frame_graph()) {
    return 20;
  }

  engine::core::Job quick{};
  quick.function = &counting_job;
  const engine::core::JobHandle handleA = engine::core::submit(quick);

  engine::core::Job reader{};
  reader.function = &signal_reading_job;
  const engine::core::JobHandle handleB = engine::core::submit(reader);
  if (!engine::core::is_valid_handle(handleA) ||
      !engine::core::is_valid_handle(handleB)) {
    return 21;
  }

  engine::core::wait(handleA);
  g_signal.store(true, std::memory_order_release);
  engine::core::wait_all();

  if (!engine::core::is_completed(handleB)) {
    return 22;
  }
  if (!g_signalObserved.load(std::memory_order_acquire)) {
    return 23;
  }

  if (!engine::core::end_frame_graph()) {
    return 24;
  }
  return 0;
}

/// wait_all must still drain every submitted job, dependencies included.
int run_wait_all_round() noexcept {
  constexpr std::uint32_t kJobCount = 64U;
  g_executedCount.store(0U, std::memory_order_relaxed);

  if (!engine::core::begin_frame_graph()) {
    return 30;
  }

  engine::core::Job job{};
  job.function = &counting_job;
  engine::core::JobHandle previous{};
  for (std::uint32_t i = 0U; i < kJobCount; ++i) {
    const engine::core::JobHandle handle = engine::core::submit(job);
    if (!engine::core::is_valid_handle(handle)) {
      return 31;
    }
    if (engine::core::is_valid_handle(previous) &&
        !engine::core::add_dependency(previous, handle)) {
      return 32;
    }
    previous = handle;
  }

  engine::core::wait_all();
  if (g_executedCount.load(std::memory_order_relaxed) != kJobCount) {
    return 33;
  }
  if (!engine::core::is_completed(previous)) {
    return 34;
  }

  if (!engine::core::end_frame_graph()) {
    return 35;
  }
  return 0;
}

/// Multi-worker smoke: wait(handle) then wait_all completes the graph
/// without hangs at every worker interleaving.
int run_worker_round() noexcept {
  g_executedCount.store(0U, std::memory_order_relaxed);

  if (!engine::core::begin_frame_graph()) {
    return 40;
  }

  engine::core::Job job{};
  job.function = &counting_job;
  const engine::core::JobHandle handleA = engine::core::submit(job);
  const engine::core::JobHandle handleB = engine::core::submit(job);
  if (!engine::core::is_valid_handle(handleA) ||
      !engine::core::is_valid_handle(handleB)) {
    return 41;
  }

  engine::core::wait(handleA);
  if (!engine::core::is_completed(handleA)) {
    return 42;
  }

  engine::core::wait_all();
  if (g_executedCount.load(std::memory_order_relaxed) != 2U) {
    return 43;
  }

  if (!engine::core::end_frame_graph()) {
    return 44;
  }
  return 0;
}

/// Runs one scenario under a fresh job system with the given worker count.
int run_with_workers(std::uint32_t workerCount, int (*round)() noexcept,
                     const char *label) noexcept {
  if (!engine::core::initialize_job_system(workerCount)) {
    std::fprintf(stderr, "job system init failed (%s)\n", label);
    return 1;
  }

  const int result = round();
  engine::core::shutdown_job_system();
  if (result != 0) {
    std::fprintf(stderr, "%s failed with code %d\n", label, result);
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = run_with_workers(0U, &run_handle_scope_round,
                                "handle scope (zero workers)");
  if (result != 0) {
    return result;
  }

  result = run_with_workers(0U, &run_deferred_signal_round,
                            "deferred signal (zero workers)");
  if (result != 0) {
    return result;
  }

  result =
      run_with_workers(0U, &run_wait_all_round, "wait_all (zero workers)");
  if (result != 0) {
    return result;
  }

  result = run_with_workers(2U, &run_wait_all_round, "wait_all (2 workers)");
  if (result != 0) {
    return result;
  }

  result = run_with_workers(4U, &run_worker_round, "worker smoke (4 workers)");
  if (result != 0) {
    return result;
  }

  return 0;
}
