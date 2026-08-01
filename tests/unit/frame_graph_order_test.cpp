// Pins the fixed-step catch-up DAG ordering contract (audit C-02): each
// catch-up step's begin job completes before any of that step's update
// jobs starts, all updates complete before the step's commit, and each
// commit completes before the next step's begin. The test wires the exact
// dependency shape engine_pipeline builds for multi-step frames onto the
// real job system and proves the scheduler cannot interleave phase
// preparation with chunk work across many randomized runs.

#include "engine/core/job_system.h"

#include <atomic>
#include <cstddef>
#include <cstdio>

namespace {

constexpr std::size_t kSteps = 3U;
constexpr std::size_t kChunks = 8U;
constexpr int kIterations = 200;

/// Per-step ordering state the marker jobs assert against.
struct StepState final {
  std::atomic<bool> beginDone{false};
  std::atomic<bool> commitDone{false};
  std::atomic<int> updatesStarted{0};
  std::atomic<int> updatesDone{0};
  std::atomic<int> violations{0};
};

/// Job payload: the step array plus this job's step index.
struct MarkerData final {
  StepState *steps = nullptr;
  std::size_t step = 0U;
};

/// Begin marker: the prior commit must be done and no update of this step
/// may have started yet.
void begin_job(void *userData) noexcept {
  auto *data = static_cast<MarkerData *>(userData);
  StepState &state = data->steps[data->step];
  if ((data->step > 0U) &&
      !data->steps[data->step - 1U].commitDone.load(
          std::memory_order_acquire)) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  if (state.updatesStarted.load(std::memory_order_acquire) != 0) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  state.beginDone.store(true, std::memory_order_release);
}

/// Update marker: the step's begin must have completed first.
void update_job(void *userData) noexcept {
  auto *data = static_cast<MarkerData *>(userData);
  StepState &state = data->steps[data->step];
  state.updatesStarted.fetch_add(1, std::memory_order_acq_rel);
  if (!state.beginDone.load(std::memory_order_acquire)) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  volatile unsigned spin = 0U;
  for (unsigned i = 0U; i < 400U; ++i) {
    spin = spin + i;
  }
  state.updatesDone.fetch_add(1, std::memory_order_acq_rel);
}

/// Commit marker: every update of the step must have completed first.
void commit_job(void *userData) noexcept {
  auto *data = static_cast<MarkerData *>(userData);
  StepState &state = data->steps[data->step];
  if (state.updatesDone.load(std::memory_order_acquire) !=
      static_cast<int>(kChunks)) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  if ((data->step > 0U) &&
      !state.beginDone.load(std::memory_order_acquire)) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  state.commitDone.store(true, std::memory_order_release);
}

/// Builds and runs one catch-up frame graph mirroring the pipeline's
/// fixed-step wiring; returns a nonzero code on the first defect.
int run_round(StepState *steps, MarkerData *markers) noexcept {
  for (std::size_t i = 0U; i < kSteps; ++i) {
    steps[i].beginDone.store(false);
    steps[i].commitDone.store(false);
    steps[i].updatesStarted.store(0);
    steps[i].updatesDone.store(0);
    steps[i].violations.store(0);
  }
  // Step 0's begin runs synchronously before submission in the pipeline.
  steps[0].beginDone.store(true, std::memory_order_release);

  if (!engine::core::begin_frame_graph()) {
    return 10;
  }

  engine::core::JobHandle previousCommit{};
  engine::core::JobHandle lastCommit{};
  std::size_t markerCursor = 0U;

  for (std::size_t step = 0U; step < kSteps; ++step) {
    MarkerData &commitData = markers[markerCursor++];
    commitData.steps = steps;
    commitData.step = step;
    engine::core::Job commit{};
    commit.function = &commit_job;
    commit.data = &commitData;
    const engine::core::JobHandle commitHandle = engine::core::submit(commit);
    if (!engine::core::is_valid_handle(commitHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 11;
    }
    if (engine::core::is_valid_handle(previousCommit) &&
        !engine::core::add_dependency(previousCommit, commitHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 12;
    }

    engine::core::JobHandle beginHandle{};
    if (step > 0U) {
      MarkerData &beginData = markers[markerCursor++];
      beginData.steps = steps;
      beginData.step = step;
      engine::core::Job begin{};
      begin.function = &begin_job;
      begin.data = &beginData;
      beginHandle = engine::core::submit(begin);
      if (!engine::core::is_valid_handle(beginHandle) ||
          !engine::core::add_dependency(previousCommit, beginHandle) ||
          !engine::core::add_dependency(beginHandle, commitHandle)) {
        static_cast<void>(engine::core::end_frame_graph());
        return 13;
      }
    }
    const engine::core::JobHandle updateGate =
        engine::core::is_valid_handle(beginHandle) ? beginHandle
                                                   : previousCommit;

    for (std::size_t chunk = 0U; chunk < kChunks; ++chunk) {
      MarkerData &updateData = markers[markerCursor++];
      updateData.steps = steps;
      updateData.step = step;
      engine::core::Job update{};
      update.function = &update_job;
      update.data = &updateData;
      const engine::core::JobHandle updateHandle =
          engine::core::submit(update);
      if (!engine::core::is_valid_handle(updateHandle)) {
        static_cast<void>(engine::core::end_frame_graph());
        return 14;
      }
      if (engine::core::is_valid_handle(updateGate) &&
          !engine::core::add_dependency(updateGate, updateHandle)) {
        static_cast<void>(engine::core::end_frame_graph());
        return 15;
      }
      if (!engine::core::add_dependency(updateHandle, commitHandle)) {
        static_cast<void>(engine::core::end_frame_graph());
        return 16;
      }
    }

    previousCommit = commitHandle;
    lastCommit = commitHandle;
  }

  engine::core::wait(lastCommit);
  if (!engine::core::end_frame_graph()) {
    return 17;
  }

  for (std::size_t i = 0U; i < kSteps; ++i) {
    if (steps[i].violations.load() != 0) {
      return 20;
    }
    if (steps[i].updatesDone.load() != static_cast<int>(kChunks)) {
      return 21;
    }
    if (!steps[i].commitDone.load()) {
      return 22;
    }
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_job_system(3U)) {
    std::fprintf(stderr, "frame_graph_order_test: job system init failed\n");
    return 1;
  }

  static StepState steps[kSteps];
  static MarkerData markers[kSteps * (kChunks + 2U)];

  int result = 0;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    result = run_round(steps, markers);
    if (result != 0) {
      std::fprintf(stderr,
                   "frame_graph_order_test failed: %d (iteration %d)\n",
                   result, iteration);
      break;
    }
  }

  engine::core::shutdown_job_system();
  if (result == 0) {
    std::printf("frame_graph_order_test: all tests passed\n");
  }
  return result;
}
