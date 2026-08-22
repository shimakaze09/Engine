// Verifies the pure frame pacing helpers: vsync interval normalization
// to the supported set, exact frame-cap wait computation (uncapped,
// under budget, exactly on budget, and over budget), frame-delta
// snapping to the fixed step within its jitter band, and the fixed-step
// count decision including the paused editor's single-step path.

#include "frame_pacing.h"

#include <cstdio>

namespace {

using engine::runtime::frame_cap_wait_seconds;
using engine::runtime::normalize_vsync_interval;

/// EXPECTATION: any negative request maps to adaptive (-1), zero stays
/// off, and every positive value clamps to plain vsync (1).
int check_vsync_normalization() {
  if (normalize_vsync_interval(-1) != -1) {
    return 1;
  }
  if (normalize_vsync_interval(-7) != -1) {
    return 2;
  }
  if (normalize_vsync_interval(0) != 0) {
    return 3;
  }
  if (normalize_vsync_interval(1) != 1) {
    return 4;
  }
  if (normalize_vsync_interval(5) != 1) {
    return 5;
  }
  return 0;
}

/// EXPECTATION: maxFps <= 0 waits nothing; a frame under budget waits
/// exactly the remainder (target 1/maxFps); a frame at or past its budget
/// waits nothing.
int check_frame_cap_wait() {
  if (frame_cap_wait_seconds(0.005, 0) != 0.0) {
    return 10;
  }
  if (frame_cap_wait_seconds(0.005, -30) != 0.0) {
    return 11;
  }

  const double target = 1.0 / 100.0;
  if (frame_cap_wait_seconds(0.0, 100) != target) {
    return 12;
  }
  if (frame_cap_wait_seconds(0.004, 100) != (target - 0.004)) {
    return 13;
  }
  if (frame_cap_wait_seconds(target, 100) != 0.0) {
    return 14;
  }
  if (frame_cap_wait_seconds(0.5, 100) != 0.0) {
    return 15;
  }

  if (frame_cap_wait_seconds(1.0 / 60.0, 60) != 0.0) {
    return 16;
  }
  return 0;
}

using engine::runtime::snap_delta_to_fixed_step;

/// EXPECTATION: deltas within 3% of the fixed step snap to exactly the
/// step (vsync jitter at the fixed rate must not oscillate the
/// accumulator); deltas outside the band, other refresh periods, and
/// degenerate steps pass through unchanged.
int check_delta_snapping() {
  const double step = 1.0 / 60.0;
  if (snap_delta_to_fixed_step(step, step) != step) {
    return 40;
  }
  if (snap_delta_to_fixed_step(step + (step * 0.02), step) != step) {
    return 41;
  }
  if (snap_delta_to_fixed_step(step - (step * 0.02), step) != step) {
    return 42;
  }
  const double outsideHigh = step * 1.05;
  if (snap_delta_to_fixed_step(outsideHigh, step) != outsideHigh) {
    return 43;
  }
  const double outsideLow = step * 0.95;
  if (snap_delta_to_fixed_step(outsideLow, step) != outsideLow) {
    return 44;
  }
  const double half = step * 0.5; // 120 Hz refresh under a 60 Hz step
  if (snap_delta_to_fixed_step(half, step) != half) {
    return 45;
  }
  if (snap_delta_to_fixed_step(0.25, 0.0) != 0.25) {
    return 46;
  }
  return 0;
}

using engine::runtime::fixed_step_decision;
using engine::runtime::FixedStepDecision;

/// EXPECTATION: a single-step frame simulates exactly one step with the
/// accumulator cleared, regardless of the accumulator's contents.
int check_single_step_decision() {
  const FixedStepDecision paused =
      fixed_step_decision(true, true, 0.0, 1.0 / 60.0, 4U);
  if ((paused.stepCount != 1U) || (paused.remainingAccumulator != 0.0)) {
    return 20;
  }

  const FixedStepDecision banked =
      fixed_step_decision(true, true, 0.5, 1.0 / 60.0, 4U);
  if ((banked.stepCount != 1U) || (banked.remainingAccumulator != 0.0)) {
    return 21;
  }
  return 0;
}

/// EXPECTATION: playing frames drain whole fixedDelta chunks (clamped to
/// maxSteps) and keep the exact remainder; non-playing frames without a
/// step request simulate nothing and clear the accumulator. Exactly
/// representable values keep every subtraction exact.
int check_playing_step_decision() {
  const double delta = 0.25;

  const FixedStepDecision idle =
      fixed_step_decision(false, false, 0.5, delta, 4U);
  if ((idle.stepCount != 0U) || (idle.remainingAccumulator != 0.0)) {
    return 30;
  }

  const FixedStepDecision under =
      fixed_step_decision(true, false, 0.125, delta, 4U);
  if ((under.stepCount != 0U) || (under.remainingAccumulator != 0.125)) {
    return 31;
  }

  const FixedStepDecision two =
      fixed_step_decision(true, false, 0.625, delta, 4U);
  if ((two.stepCount != 2U) || (two.remainingAccumulator != 0.125)) {
    return 32;
  }

  const FixedStepDecision clamped =
      fixed_step_decision(true, false, 10.0, delta, 4U);
  if ((clamped.stepCount != 4U) || (clamped.remainingAccumulator != 0.0)) {
    return 33;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_vsync_normalization();
  if (result != 0) {
    std::printf("vsync normalization failed: %d\n", result);
    return result;
  }
  result = check_frame_cap_wait();
  if (result != 0) {
    std::printf("frame cap wait failed: %d\n", result);
    return result;
  }
  result = check_delta_snapping();
  if (result != 0) {
    std::printf("delta snapping failed: %d\n", result);
    return result;
  }
  result = check_single_step_decision();
  if (result != 0) {
    std::printf("single step decision failed: %d\n", result);
    return result;
  }
  result = check_playing_step_decision();
  if (result != 0) {
    std::printf("playing step decision failed: %d\n", result);
    return result;
  }
  return 0;
}
