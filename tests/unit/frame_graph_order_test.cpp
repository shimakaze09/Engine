// Pins the fixed-step catch-up DAG ordering contract (audit C-02) and the
// split-frame contract (issue #78): each catch-up step's begin job completes
// before any of that step's update jobs starts, all updates complete before
// the step's commit, and each commit completes before the next step's begin;
// the frame then splits into two graphs — the simulation graph is waited out
// after the last commit, the main thread publishes the camera, and only then
// does the render-prep graph run, so every render-prep job observes the
// frame's camera. The test wires the exact dependency shape engine_pipeline
// builds onto the real job system across many randomized runs.

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

/// Per-step state for the zero-chunk variant: with no update jobs, the
/// resolve job must still order after the step's begin.
struct ZeroChunkStepState final {
  std::atomic<bool> beginDone{false};
  std::atomic<bool> resolveDone{false};
  std::atomic<bool> commitDone{false};
  std::atomic<int> violations{0};
};

/// Zero-chunk payload: the step array plus this job's step index.
struct ZeroChunkMarkerData final {
  ZeroChunkStepState *steps = nullptr;
  std::size_t step = 0U;
};

/// Zero-chunk begin marker: the prior commit must already be done.
void zero_chunk_begin_job(void *userData) noexcept {
  auto *data = static_cast<ZeroChunkMarkerData *>(userData);
  ZeroChunkStepState &state = data->steps[data->step];
  if ((data->step > 0U) &&
      !data->steps[data->step - 1U].commitDone.load(
          std::memory_order_acquire)) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  if (state.resolveDone.load(std::memory_order_acquire)) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  state.beginDone.store(true, std::memory_order_release);
}

/// Zero-chunk resolve marker: the step's begin must have completed.
void zero_chunk_resolve_job(void *userData) noexcept {
  auto *data = static_cast<ZeroChunkMarkerData *>(userData);
  ZeroChunkStepState &state = data->steps[data->step];
  if (!state.beginDone.load(std::memory_order_acquire)) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  state.resolveDone.store(true, std::memory_order_release);
}

/// Zero-chunk commit marker: the step's resolve must have completed.
void zero_chunk_commit_job(void *userData) noexcept {
  auto *data = static_cast<ZeroChunkMarkerData *>(userData);
  ZeroChunkStepState &state = data->steps[data->step];
  if (!state.resolveDone.load(std::memory_order_acquire)) {
    state.violations.fetch_add(1, std::memory_order_relaxed);
  }
  state.commitDone.store(true, std::memory_order_release);
}

/// Builds one catch-up frame with ZERO update chunks per step, mirroring
/// the pipeline's wiring where resolve gates directly on the step begin;
/// returns a nonzero code on the first defect.
int run_zero_chunk_round(ZeroChunkStepState *steps,
                         ZeroChunkMarkerData *markers) noexcept {
  for (std::size_t i = 0U; i < kSteps; ++i) {
    steps[i].beginDone.store(false);
    steps[i].resolveDone.store(false);
    steps[i].commitDone.store(false);
    steps[i].violations.store(0);
  }
  steps[0].beginDone.store(true, std::memory_order_release);

  if (!engine::core::begin_frame_graph()) {
    return 40;
  }

  engine::core::JobHandle previousCommit{};
  engine::core::JobHandle lastCommit{};
  std::size_t markerCursor = 0U;

  for (std::size_t step = 0U; step < kSteps; ++step) {
    ZeroChunkMarkerData &commitData = markers[markerCursor++];
    commitData.steps = steps;
    commitData.step = step;
    engine::core::Job commit{};
    commit.function = &zero_chunk_commit_job;
    commit.data = &commitData;
    const engine::core::JobHandle commitHandle = engine::core::submit(commit);
    if (!engine::core::is_valid_handle(commitHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 41;
    }
    if (engine::core::is_valid_handle(previousCommit) &&
        !engine::core::add_dependency(previousCommit, commitHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 42;
    }

    engine::core::JobHandle beginHandle{};
    if (step > 0U) {
      ZeroChunkMarkerData &beginData = markers[markerCursor++];
      beginData.steps = steps;
      beginData.step = step;
      engine::core::Job begin{};
      begin.function = &zero_chunk_begin_job;
      begin.data = &beginData;
      beginHandle = engine::core::submit(begin);
      if (!engine::core::is_valid_handle(beginHandle) ||
          !engine::core::add_dependency(previousCommit, beginHandle) ||
          !engine::core::add_dependency(beginHandle, commitHandle)) {
        static_cast<void>(engine::core::end_frame_graph());
        return 43;
      }
    }
    const engine::core::JobHandle updateGate =
        engine::core::is_valid_handle(beginHandle) ? beginHandle
                                                   : previousCommit;

    ZeroChunkMarkerData &resolveData = markers[markerCursor++];
    resolveData.steps = steps;
    resolveData.step = step;
    engine::core::Job resolve{};
    resolve.function = &zero_chunk_resolve_job;
    resolve.data = &resolveData;
    const engine::core::JobHandle resolveHandle =
        engine::core::submit(resolve);
    if (!engine::core::is_valid_handle(resolveHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 44;
    }
    if (engine::core::is_valid_handle(updateGate) &&
        !engine::core::add_dependency(updateGate, resolveHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 45;
    }
    if (!engine::core::add_dependency(resolveHandle, commitHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 46;
    }

    previousCommit = commitHandle;
    lastCommit = commitHandle;
  }

  engine::core::wait(lastCommit);
  if (!engine::core::end_frame_graph()) {
    return 47;
  }

  for (std::size_t i = 0U; i < kSteps; ++i) {
    if (steps[i].violations.load() != 0) {
      return 48;
    }
    if (!steps[i].resolveDone.load() || !steps[i].commitDone.load()) {
      return 49;
    }
  }
  return 0;
}

/// Shared state for the split-frame round: sim graph, then main-thread
/// camera publish, then the render-prep graph.
struct SplitFrameState final {
  std::atomic<bool> lastCommitDone{false};
  std::atomic<bool> cameraPublished{false};
  std::atomic<bool> prepBeginDone{false};
  std::atomic<int> prepChunksDone{0};
  std::atomic<int> violations{0};
};

/// Sim commit marker for the split round; the last one flags completion.
void split_commit_job(void *userData) noexcept {
  auto *state = static_cast<SplitFrameState *>(userData);
  if (state->cameraPublished.load(std::memory_order_acquire)) {
    state->violations.fetch_add(1, std::memory_order_relaxed);
  }
  state->lastCommitDone.store(true, std::memory_order_release);
}

/// Render-prep phase marker: the camera must already be published.
void split_prep_begin_job(void *userData) noexcept {
  auto *state = static_cast<SplitFrameState *>(userData);
  if (!state->cameraPublished.load(std::memory_order_acquire) ||
      !state->lastCommitDone.load(std::memory_order_acquire)) {
    state->violations.fetch_add(1, std::memory_order_relaxed);
  }
  state->prepBeginDone.store(true, std::memory_order_release);
}

/// Render-prep chunk marker: the prep phase begin must have completed.
void split_prep_chunk_job(void *userData) noexcept {
  auto *state = static_cast<SplitFrameState *>(userData);
  if (!state->prepBeginDone.load(std::memory_order_acquire) ||
      !state->cameraPublished.load(std::memory_order_acquire)) {
    state->violations.fetch_add(1, std::memory_order_relaxed);
  }
  state->prepChunksDone.fetch_add(1, std::memory_order_acq_rel);
}

/// Runs one split frame mirroring the pipeline: simulation graph waited out
/// after the last commit, main-thread camera publish, render-prep graph.
int run_split_frame_round(SplitFrameState *state) noexcept {
  state->lastCommitDone.store(false);
  state->cameraPublished.store(false);
  state->prepBeginDone.store(false);
  state->prepChunksDone.store(0);
  state->violations.store(0);

  if (!engine::core::begin_frame_graph()) {
    return 60;
  }

  engine::core::JobHandle previousCommit{};
  for (std::size_t step = 0U; step < kSteps; ++step) {
    engine::core::Job commit{};
    commit.function = &split_commit_job;
    commit.data = state;
    const engine::core::JobHandle commitHandle = engine::core::submit(commit);
    if (!engine::core::is_valid_handle(commitHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 61;
    }
    if (engine::core::is_valid_handle(previousCommit) &&
        !engine::core::add_dependency(previousCommit, commitHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 62;
    }
    previousCommit = commitHandle;
  }

  engine::core::wait(previousCommit);
  if (!engine::core::end_frame_graph()) {
    return 63;
  }

  if (!state->lastCommitDone.load(std::memory_order_acquire)) {
    return 64;
  }
  state->cameraPublished.store(true, std::memory_order_release);

  if (!engine::core::begin_frame_graph()) {
    return 65;
  }

  engine::core::Job prepBegin{};
  prepBegin.function = &split_prep_begin_job;
  prepBegin.data = state;
  const engine::core::JobHandle prepBeginHandle =
      engine::core::submit(prepBegin);
  if (!engine::core::is_valid_handle(prepBeginHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    return 66;
  }

  engine::core::JobHandle chunkHandles[kChunks]{};
  for (std::size_t chunk = 0U; chunk < kChunks; ++chunk) {
    engine::core::Job prepChunk{};
    prepChunk.function = &split_prep_chunk_job;
    prepChunk.data = state;
    chunkHandles[chunk] = engine::core::submit(prepChunk);
    if (!engine::core::is_valid_handle(chunkHandles[chunk]) ||
        !engine::core::add_dependency(prepBeginHandle, chunkHandles[chunk])) {
      static_cast<void>(engine::core::end_frame_graph());
      return 67;
    }
  }

  for (std::size_t chunk = 0U; chunk < kChunks; ++chunk) {
    engine::core::wait(chunkHandles[chunk]);
  }
  if (!engine::core::end_frame_graph()) {
    return 68;
  }

  if (state->prepChunksDone.load() != static_cast<int>(kChunks)) {
    return 70;
  }
  if (state->violations.load() != 0) {
    return 69;
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
  static ZeroChunkStepState zeroChunkSteps[kSteps];
  static ZeroChunkMarkerData zeroChunkMarkers[kSteps * 3U];
  static SplitFrameState splitFrameState;

  int result = 0;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    result = run_round(steps, markers);
    if (result != 0) {
      std::fprintf(stderr,
                   "frame_graph_order_test failed: %d (iteration %d)\n",
                   result, iteration);
      break;
    }
    result = run_zero_chunk_round(zeroChunkSteps, zeroChunkMarkers);
    if (result != 0) {
      std::fprintf(stderr,
                   "frame_graph_order_test failed: %d (iteration %d)\n",
                   result, iteration);
      break;
    }
    result = run_split_frame_round(&splitFrameState);
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
