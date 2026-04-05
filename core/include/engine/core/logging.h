#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

enum class LogLevel : std::uint8_t {
    Trace,
    Info,
    Warning,
    Error,
    Fatal
};

bool initialize_logging() noexcept;
void shutdown_logging() noexcept;
void log_message(LogLevel level, const char* channel, const char* message) noexcept;
void log_frame_metrics(
    std::uint32_t frameIndex,
    double frameMs,
    std::size_t frameBytes,
    std::size_t frameAllocations) noexcept;

} // namespace engine::core
