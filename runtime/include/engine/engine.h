// Declares engine types and APIs for the Engine runtime world.

#pragma once

#include <cstdint>

namespace engine {

/// Handles bootstrap.
bool bootstrap() noexcept;
/// Runs the configured command, loop, or tool.
void run(std::uint32_t maxFrames = 0U) noexcept;
/// Shuts down the owning system.
void shutdown() noexcept;

} // namespace engine
