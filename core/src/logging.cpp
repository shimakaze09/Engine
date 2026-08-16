// Implements logging behavior for the Engine core engine.

#include "engine/core/logging.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>


namespace engine::core {

namespace {

std::atomic<bool> g_loggingInitialized{false};
std::atomic<std::uint32_t> g_frameIndex{0U};

/// One fixed sink table slot; unused slots have fn == nullptr.
struct SinkSlot final {
  LogSinkFn fn = nullptr;
  void *userData = nullptr;
};

std::mutex g_sinkMutex{};
std::array<SinkSlot, kMaxLogSinks> g_sinks{};

/// Calls every registered sink with the given event; the sink table lock is
/// held only for the fixed-size iteration, never across a sink callback's
/// own external work, keeping this dispatch lock-light.
void dispatch_to_sinks(LogLevel level, const char *channel,
                       const char *message) noexcept {
  std::array<SinkSlot, kMaxLogSinks> snapshot{};
  {
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    snapshot = g_sinks;
  }
  for (const SinkSlot &slot : snapshot) {
    if (slot.fn != nullptr) {
      slot.fn(level, channel, message, slot.userData);
    }
  }
}

} // namespace

const char *log_level_to_string(LogLevel level) noexcept {
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

/// Initializes the owning system for logging.
bool initialize_logging() noexcept {
  g_loggingInitialized.store(true, std::memory_order_release);
  return true;
}

/// Shuts down the owning system for logging.
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

  std::printf("[%s][%s][%s] %s\n", timestamp, log_level_to_string(level),
             channel, message);

  // Sinks run even for Fatal so an editor-side capture still records the
  // message that is about to abort the process (the Fatal-only-abort
  // contract governs process exit, not diagnostic capture).
  dispatch_to_sinks(level, channel, message);

  if (level == LogLevel::Fatal) {
    std::fflush(stdout);
    std::abort();
  }
}

void log_set_frame_index(std::uint32_t frameIndex) noexcept {
  g_frameIndex.store(frameIndex, std::memory_order_relaxed);
}

std::uint32_t log_current_frame_index() noexcept {
  return g_frameIndex.load(std::memory_order_relaxed);
}

bool log_register_sink(LogSinkFn fn, void *userData) noexcept {
  if (fn == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_sinkMutex);
  bool hasFreeSlot = false;
  std::size_t freeIndex = 0U;
  for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
    if ((g_sinks[i].fn == fn) && (g_sinks[i].userData == userData)) {
      return false; // already registered
    }
    if (!hasFreeSlot && (g_sinks[i].fn == nullptr)) {
      hasFreeSlot = true;
      freeIndex = i;
    }
  }
  if (!hasFreeSlot) {
    return false;
  }
  g_sinks[freeIndex] = SinkSlot{fn, userData};
  return true;
}

void log_unregister_sink(LogSinkFn fn, void *userData) noexcept {
  std::lock_guard<std::mutex> lock(g_sinkMutex);
  for (SinkSlot &slot : g_sinks) {
    if ((slot.fn == fn) && (slot.userData == userData)) {
      slot = SinkSlot{};
      return;
    }
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
