// Implements frame pacing helpers: vsync interval normalization, the
// pure frame-cap wait computation, the fixed-step count decision, and the
// hybrid sleep-then-spin wait the pipeline runs at the end of each frame.

#include "frame_pacing.h"

#include <chrono>
#include <thread>

namespace engine::runtime {

int normalize_vsync_interval(int requested) noexcept {
  if (requested < 0) {
    return -1;
  }
  return (requested == 0) ? 0 : 1;
}

double frame_cap_wait_seconds(double elapsedSeconds, int maxFps) noexcept {
  if (maxFps <= 0) {
    return 0.0;
  }
  const double targetSeconds = 1.0 / static_cast<double>(maxFps);
  const double wait = targetSeconds - elapsedSeconds;
  return (wait > 0.0) ? wait : 0.0;
}

double snap_delta_to_fixed_step(double deltaSeconds,
                                double fixedDeltaSeconds) noexcept {
  if (fixedDeltaSeconds <= 0.0) {
    return deltaSeconds;
  }
  // 3% of a 60 Hz step is 0.5 ms: wide enough for present/acquire
  // jitter, narrow enough that a real 58/62 Hz rate stays unsnapped
  // and simulation time cannot drift against the wall clock.
  const double tolerance = fixedDeltaSeconds * 0.03;
  const double difference = deltaSeconds - fixedDeltaSeconds;
  if ((difference >= -tolerance) && (difference <= tolerance)) {
    return fixedDeltaSeconds;
  }
  return deltaSeconds;
}

FixedStepDecision fixed_step_decision(bool playing, bool singleStep,
                                      double accumulatorSeconds,
                                      double fixedDeltaSeconds,
                                      std::size_t maxSteps) noexcept {
  FixedStepDecision decision{};
  if (singleStep) {
    decision.stepCount = 1U;
    return decision;
  }
  if (!playing || (fixedDeltaSeconds <= 0.0)) {
    return decision;
  }

  double accumulator = accumulatorSeconds;
  const double maxAccumulator =
      static_cast<double>(maxSteps) * fixedDeltaSeconds;
  if (accumulator > maxAccumulator) {
    accumulator = maxAccumulator;
  }
  while ((accumulator >= fixedDeltaSeconds) &&
         (decision.stepCount < maxSteps)) {
    accumulator -= fixedDeltaSeconds;
    ++decision.stepCount;
  }
  decision.remainingAccumulator = accumulator;
  return decision;
}

void wait_for_frame_cap(double waitSeconds) noexcept {
  if (waitSeconds <= 0.0) {
    return;
  }

  using Clock = std::chrono::steady_clock;
  const Clock::time_point deadline =
      Clock::now() + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>(waitSeconds));

  // OS sleep granularity is coarse (~1.5 ms on Windows), so sleep most of
  // the wait and spin the remainder.
  constexpr std::chrono::milliseconds kSpinReserve{2};
  const Clock::time_point sleepUntil = deadline - kSpinReserve;
  if (sleepUntil > Clock::now()) {
    std::this_thread::sleep_until(sleepUntil);
  }
  while (Clock::now() < deadline) {
    std::this_thread::yield();
  }
}

} // namespace engine::runtime
