#include "engine/core/logging.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>


namespace engine::core {

namespace {

std::atomic<bool> g_loggingInitialized{false};

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
  g_loggingInitialized.store(true, std::memory_order_release);
  return true;
}

void shutdown_logging() noexcept {
  g_loggingInitialized.store(false, std::memory_order_release);
}

void log_message(LogLevel level,
                 const char *channel,
                 const char *message) noexcept {
  if (!g_loggingInitialized.load(std::memory_order_acquire)) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()) % 1000;
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmInfo{};
#if defined(_WIN32)
  localtime_s(&tmInfo, &t);
#else
  localtime_r(&t, &tmInfo);
#endif
  char timestamp[24] = {};
  std::snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%03d",
      tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec,
      static_cast<int>(ms.count()));

  std::printf("[%s][%s][%s] %s\n", timestamp, to_string(level), channel, message);

  if (level == LogLevel::Fatal) {
    std::fflush(stdout);
    std::abort();
  }
}

void log_frame_metrics(std::uint32_t frameIndex,
                       double frameMs,
                       std::size_t frameBytes,
                       std::size_t frameAllocations) noexcept {
  if (!g_loggingInitialized.load(std::memory_order_acquire)) {
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
