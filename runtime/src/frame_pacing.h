// Declares frame pacing helpers for the engine pipeline: vsync interval
// normalization, the pure frame-cap wait computation and fixed-step count
// decision (unit-tested exactly; the wall clock stays in the pipeline),
// and the hybrid sleep-then-spin wait.

#pragma once

#include <cstddef>

namespace engine::runtime {

/// Clamps a vsync cvar value to a supported present interval:
/// negative -> -1 (adaptive), 0 -> off, anything else -> 1.
int normalize_vsync_interval(int requested) noexcept;

/// Seconds left to wait so the frame spans 1/maxFps seconds; 0 when
/// uncapped (maxFps <= 0) or the frame already ran past its budget.
double frame_cap_wait_seconds(double elapsedSeconds, int maxFps) noexcept;

/// Snaps a measured frame delta to the fixed step when they differ by
/// at most 3% of the step. When the display refresh equals the fixed
/// rate (vsync at 60 Hz), the true period IS the step and the measured
/// spread is sampling noise; feeding it raw leaves the accumulator on
/// the drain boundary and the step count alternates 0/2 (visible
/// stutter). Deltas outside the band (other refresh rates, uncapped
/// runs, hitches) pass through unchanged.
double snap_delta_to_fixed_step(double deltaSeconds,
                                double fixedDeltaSeconds) noexcept;

/// Blocks for waitSeconds using a coarse sleep followed by a spin so the
/// cap stays precise despite OS timer granularity. No-op for waits <= 0.
void wait_for_frame_cap(double waitSeconds) noexcept;

/// How many fixed steps one frame simulates and the accumulator it keeps.
struct FixedStepDecision final {
  std::size_t stepCount = 0U;
  double remainingAccumulator = 0.0;
};

/// Computes the frame's fixed-step count: a single-step frame (the paused
/// editor's Step button) simulates exactly one step with the accumulator
/// cleared; playing frames drain the accumulator in fixedDelta chunks
/// after clamping it to maxSteps * fixedDelta; every other frame simulates
/// none and clears the accumulator.
FixedStepDecision fixed_step_decision(bool playing, bool singleStep,
                                      double accumulatorSeconds,
                                      double fixedDeltaSeconds,
                                      std::size_t maxSteps) noexcept;

} // namespace engine::runtime
