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

/// Publishes the clock with a new frame index.
inline void publish_frame_index(std::uint32_t frameIndex) noexcept {
  core::SimulationClock clock = scripting::simulation_clock();
  clock.frameIndex = frameIndex;
  scripting::set_simulation_clock(clock);
}

} // namespace engine::tests
