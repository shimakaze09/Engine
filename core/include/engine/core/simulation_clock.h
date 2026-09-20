// Declares the simulation clock: the one value the engine pipeline
// publishes per frame to every consumer of simulated time, so scripts,
// timers and coroutines read a single consistent view of the fixed step
// instead of separately mirrored globals.

#pragma once

#include <cstdint>

namespace engine::core {

/// The fixed simulation step every per-step system integrates with.
inline constexpr double kFixedDeltaSeconds = 1.0 / 60.0;

/// Where the simulation stands at the moment of publication. A run starts
/// at the zero clock; every field is derived from the fixed step and the
/// frame count, never from the wall clock, so a replay that feeds the same
/// frame deltas reproduces the same clocks.
struct SimulationClock final {
  /// Fixed steps simulated since the run (or the current play) began.
  std::uint64_t tickIndex = 0U;
  /// Rendered frames since the run began.
  std::uint32_t frameIndex = 0U;
  /// Fixed steps simulated in the current frame (0 while paused).
  std::uint32_t stepsThisFrame = 0U;
  /// Length of one fixed step.
  double fixedDeltaSeconds = kFixedDeltaSeconds;
  /// Time simulated in the current frame: stepsThisFrame * fixedDelta, the
  /// dt every once-per-frame gameplay system receives.
  double deltaSeconds = 0.0;
  /// Total simulated time since the run (or the current play) began.
  double simulationSeconds = 0.0;
  /// Fraction of a fixed step the render pose is ahead of the last step.
  double renderAlpha = 1.0;
};

} // namespace engine::core
