// Declares logging types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Enumerates log level values used by the engine.
enum class LogLevel : std::uint8_t {
    Trace,
    Info,
    Warning,
    Error,
    Fatal
};

/// Initializes the owning system for logging.
bool initialize_logging() noexcept;
/// Shuts down the owning system for logging.
void shutdown_logging() noexcept;
/// Writes one log line (level + channel + message) to the sinks.
void log_message(LogLevel level, const char* channel, const char* message) noexcept;
/// Logs the per-frame timing/metrics line.
void log_frame_metrics(
    std::uint32_t frameIndex,
    double frameMs,
    std::size_t frameBytes,
    std::size_t frameAllocations) noexcept;

} // namespace engine::core
