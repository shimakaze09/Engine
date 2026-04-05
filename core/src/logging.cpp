#include "engine/core/logging.h"

#include <cstdio>
#include <cstdlib>


namespace engine::core {

namespace {

bool g_loggingInitialized = false;

const char *to_string(LogLevel level) noexcept {
  switch (level) {
  case LogLevel::Trace:
    return "Trace";
  case LogLevel::Info:
    return "Info";
  case LogLevel::Warning:
    return "Warning";
  case LogLevel::Error:
    return "Error";
  case LogLevel::Fatal:
    return "Fatal";
  default:
    return "Unknown";
  }
}

} // namespace

bool initialize_logging() noexcept {
  g_loggingInitialized = true;
  return true;
}

void shutdown_logging() noexcept {
  g_loggingInitialized = false;
}

void log_message(LogLevel level,
                 const char *channel,
                 const char *message) noexcept {
  if (!g_loggingInitialized) {
    return;
  }

  std::printf("[%s][%s] %s\n", to_string(level), channel, message);

  if (level == LogLevel::Fatal) {
    std::fflush(stdout);
    std::abort();
  }
}

void log_frame_metrics(std::uint32_t frameIndex,
                       double frameMs,
                       std::size_t frameBytes,
                       std::size_t frameAllocations) noexcept {
  if (!g_loggingInitialized) {
    return;
  }

  std::printf(
      "[Trace][frame] index=%u ms=%.3f frameBytes=%zu frameAllocs=%zu\n",
      frameIndex,
      frameMs,
      frameBytes,
      frameAllocations);
}

} // namespace engine::core
