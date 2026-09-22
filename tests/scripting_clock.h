// Test-side composition of the scripting clock: production publishes one
// SimulationClock per frame, while a test driving scripting directly sets
// the frame time or the frame index alone and keeps the rest of the clock
// as it was.

#pragma once

#include <cstdint>

#include "engine/core/simulation_clock.h"
#include "engine/scripting/scripting.h"

namespace engine::tests {

/// Publishes the clock with a new frame delta and elapsed time.
inline void publish_frame_time(float deltaSeconds,
                               float totalSeconds) noexcept {
  core::SimulationClock clock = scripting::simulation_clock();
  clock.deltaSeconds = deltaSeconds;
  clock.simulationSeconds = totalSeconds;
  scripting::set_simulation_clock(clock);
}

/// Publishes the clock with a new frame index, and a tick index to match.
///
/// The two move together because these tests drive one fixed step per
/// frame, which is what production does at the target rate, and because
/// coroutine waits are counted in ticks (docs/decisions/0019) while the
/// instruction-budget refill is counted in frames. A test that advanced
/// only one of them would be asserting against half a clock.
inline void publish_frame_index(std::uint32_t frameIndex) noexcept {
  core::SimulationClock clock = scripting::simulation_clock();
  clock.frameIndex = frameIndex;
  clock.tickIndex = frameIndex;
  scripting::set_simulation_clock(clock);
}

/// Publishes a tick index on its own, for a test that separates
/// simulation steps from rendered frames — several steps in one frame, or
/// a frame that stepped none.
inline void publish_tick_index(std::uint64_t tickIndex) noexcept {
  core::SimulationClock clock = scripting::simulation_clock();
  clock.tickIndex = tickIndex;
  scripting::set_simulation_clock(clock);
}

} // namespace engine::tests
