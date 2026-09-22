// Drives the worker-pool rollbacks that a thread-spawn refusal takes, in
// the job system and the asset-streaming queue. Both were written for a
// real refusal -- the OS out of threads or out of memory -- which no test
// can arrange, so until ThreadOps existed neither branch had ever run
// (#416 item 7, and the same gap named in #656).
//
// What a rollback has to leave behind is the point: initialization
// reports failure, spawning stops where it was refused, the running flag
// is clear, and nothing is left for a later initialize to trip over. Each
// pool is re-initialized for real afterwards and then used, because a
// worker the rollback failed to join is invisible at the moment of the
// failure and shows up only in what happens next.
//
// The two pools are covered to different depths, and the difference is
// worth knowing rather than glossing:
//
//   The job system's rollback is fully covered. Removing its join loop
//   makes this test fail five ways, including the later real initialize,
//   because leaked workers of the previous attempt are still consuming
//   from the queue.
//
//   The streaming queue's is not. Its rollback signals workerStopRequested
//   and notifies before joining, so a worker it failed to join exits by
//   itself; removing the join loop leaves this test passing. What is lost
//   is the thread's stack, never reclaimed until the process ends -- a
//   leak, not a behaviour change, and only a leak checker can see it. The
//   streaming cases here prove initialize fails, spawning stops at the
//   refusal, and teardown after a rollback is clean; they do not prove the
//   join happens. The sanitizer lane is where that belongs.

#include "engine/content/asset_streaming.h"
#include "engine/core/job_system.h"
#include "engine/core/native_thread.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

// Which spawn to refuse, and how many were attempted. The driver is
// single-threaded and pools spawn from the calling thread, so plain
// globals are enough.
std::uint32_t g_refuseAtCall = 0U;
std::uint32_t g_spawnCalls = 0U;

/// Spawns for real until the chosen call, then refuses. Refusing for real
/// means leaving the thread unstarted, exactly as NativeThread::spawn
/// does on failure -- a refusal that started the thread anyway would have
/// the caller join something it thinks never ran.
bool counting_spawn(engine::core::NativeThread *thread,
                    engine::core::NativeThread::EntryFn entry,
                    void *userData) noexcept {
  ++g_spawnCalls;
  if (g_spawnCalls == g_refuseAtCall) {
    return false;
  }
  return (thread != nullptr) && thread->spawn(entry, userData);
}

const engine::core::ThreadOps kCountingOps{&counting_spawn};

/// Runs the job system's rollback for a refusal at `refuseAt`, and
/// returns whether everything the rollback promises held.
bool job_system_rolls_back(std::uint32_t workerCount,
                           std::uint32_t refuseAt) noexcept {
  g_refuseAtCall = refuseAt;
  g_spawnCalls = 0U;

  const bool initialized =
      engine::core::initialize_job_system(workerCount, kCountingOps);
  if (initialized) {
    std::fprintf(stderr, "FAIL: initialize reported success though spawn "
                         "%u of %u was refused\n",
                 refuseAt, workerCount);
    engine::core::shutdown_job_system();
    return false;
  }
  if (engine::core::is_job_system_initialized()) {
    std::fprintf(stderr, "FAIL: the job system reports initialized after a "
                         "refused spawn\n");
    return false;
  }
  // The refusal stopped the loop where it happened rather than trying the
  // rest: a pool that kept spawning after one refusal would leave the
  // workers it started after the rollback joined the earlier ones.
  if (g_spawnCalls != refuseAt) {
    std::fprintf(stderr,
                 "FAIL: %u spawns attempted, expected to stop at %u\n",
                 g_spawnCalls, refuseAt);
    return false;
  }
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  // --- The job system, refusing at each position a rollback can start
  // from: the first worker (nothing to join), a middle one (some joined),
  // and the last (all but one joined).
  constexpr std::uint32_t kWorkers = 4U;
  CHECK(job_system_rolls_back(kWorkers, 1U), "job system, first worker");
  CHECK(job_system_rolls_back(kWorkers, 2U), "job system, second worker");
  CHECK(job_system_rolls_back(kWorkers, kWorkers), "job system, last worker");

  // A refusal must leave nothing behind: this initialize is the real one,
  // and it has to work. If a rolled-back worker were still alive, or the
  // running flag still set, this is where it would show.
  g_refuseAtCall = 0U;
  g_spawnCalls = 0U;
  CHECK(engine::core::initialize_job_system(kWorkers, kCountingOps),
        "the job system initializes after the rollbacks");
  CHECK(engine::core::is_job_system_initialized(),
        "and reports itself initialized");

  // Used, not just initialized: a frame graph that runs proves the
  // workers are the ones this initialize started.
  CHECK(engine::core::begin_frame_graph(), "a frame graph opens");
  static int counter = 0;
  const engine::core::JobHandle handle = engine::core::submit(
      engine::core::Job{[](void *data) noexcept {
                          ++(*static_cast<int *>(data));
                        },
                        &counter});
  CHECK(handle.id != 0U, "a job is accepted");
  engine::core::wait_all();
  CHECK(counter == 1, "the job ran on a worker from the live pool");
  static_cast<void>(engine::core::end_frame_graph());
  engine::core::shutdown_job_system();

  // --- The streaming queue, same three positions. Its pool size is
  // fixed, so the last position is kWorkerCount.
  constexpr std::uint32_t kStreamingWorkers =
      static_cast<std::uint32_t>(engine::content::AssetStreamingQueue::
                                     kWorkerCount);
  for (std::uint32_t refuseAt = 1U; refuseAt <= kStreamingWorkers;
       ++refuseAt) {
    g_refuseAtCall = refuseAt;
    g_spawnCalls = 0U;
    engine::content::AssetStreamingQueue queue{};
    const bool initialized =
        engine::content::initialize_asset_streaming(&queue, kCountingOps);
    CHECK(!initialized, "streaming initialize fails on a refused spawn");
    CHECK(g_spawnCalls == refuseAt,
          "streaming stops spawning at the refusal");
    // The destructor runs shutdown_asset_streaming, so this scope exiting
    // without a hang or a fault is the check that a rolled-back queue is
    // still safe to destroy. It is not a check that the rollback joined
    // -- see the note at the top of this file.
  }

  {
    // And the real one still works afterwards.
    g_refuseAtCall = 0U;
    g_spawnCalls = 0U;
    engine::content::AssetStreamingQueue queue{};
    CHECK(engine::content::initialize_asset_streaming(&queue, kCountingOps),
          "streaming initializes after the rollbacks");
    CHECK(g_spawnCalls == kStreamingWorkers,
          "every streaming worker spawned");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("thread_spawn_rollback_test passed");
  return 0;
}
