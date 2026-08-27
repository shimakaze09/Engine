// Implements logging behavior for the Engine core engine.

#include "engine/core/logging.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

/// Dispatches that snapshotted each slot and have not finished their walk,
/// guarded by g_sinkMutex. A slot is quiescent at zero, which is the point at
/// which its owner's userData can no longer be reached through a dispatch
/// snapshot; removal waits for that, so unregister doubles as the lifetime
/// barrier.
std::array<std::uint32_t, kMaxLogSinks> g_sinkActive{};

/// Signalled by a dispatch that has finished calling its snapshot, waking the
/// removals waiting for the slots it held.
std::condition_variable g_sinkQuiescent{};

/// The calling thread's own share of g_sinkActive. Quiescence checks subtract
/// it so a sink that removes itself (or the logging system) from inside its
/// own callback is not waiting on the dispatch that is running it.
thread_local std::array<std::uint32_t, kMaxLogSinks> t_sinkActiveOnThread{};

/// True when no dispatch except the calling thread's own can still reach the
/// slot. Only the caller's share is subtracted, so a thread waiting here still
/// waits out every other thread's dispatch of this slot. Callers must hold
/// g_sinkMutex.
bool slot_quiescent_for_caller(std::size_t index) noexcept {
  return g_sinkActive[index] <= t_sinkActiveOnThread[index];
}

/// Blocks until every removed slot is quiescent for the caller, then returns.
/// A dispatch releases its claim on all the slots it snapshotted at once, so
/// this spans a whole dispatch of the table rather than one callback; the sink
/// contract in logging.h — fixed-size, non-blocking work — is what bounds it.
void wait_for_slots_quiescent(std::unique_lock<std::mutex> &lock,
                              std::size_t first, std::size_t last) noexcept {
  g_sinkQuiescent.wait(lock, [first, last]() noexcept {
    for (std::size_t i = first; i < last; ++i) {
      if (!slot_quiescent_for_caller(i)) {
        return false;
      }
    }
    return true;
  });
}

/// Calls every registered sink with the given event. The sink table lock is
/// held only for the fixed-size iteration, never across a sink callback's own
/// external work, keeping this dispatch lock-light; the snapshot the callbacks
/// run from is kept honest by counting each call in g_sinkActive, so a
/// concurrent unregister cannot retire a sink's userData mid-walk.
void dispatch_to_sinks(LogLevel level, const char *channel,
                       const char *message) noexcept {
  std::array<SinkSlot, kMaxLogSinks> snapshot{};
  {
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    snapshot = g_sinks;
    for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
      if (snapshot[i].fn != nullptr) {
        ++g_sinkActive[i];
        ++t_sinkActiveOnThread[i];
      }
    }
  }
  for (const SinkSlot &slot : snapshot) {
    if (slot.fn != nullptr) {
      slot.fn(level, channel, message, slot.userData);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
      if (snapshot[i].fn != nullptr) {
        --g_sinkActive[i];
        --t_sinkActiveOnThread[i];
      }
    }
  }
  // Notified outside the lock; the counts above drop under it, so a waiter
  // holding the lock cannot miss this wakeup.
  g_sinkQuiescent.notify_all();
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
  // Drop any sink its owner failed to unregister (#236) so a dead sink is
  // never dispatched to after a later re-initialization. Teardown owes the
  // same lifetime barrier as unregister: it returns only once no dispatch is
  // still inside one of the sinks it just dropped.
  std::unique_lock<std::mutex> lock(g_sinkMutex);
  g_sinks = {};
  wait_for_slots_quiescent(lock, 0U, kMaxLogSinks);
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
    // A slot cleared by a removal that is still waiting for quiescence is not
    // free yet: reusing it would fold a new sink's dispatches into the count
    // the departing owner is waiting on.
    if (!hasFreeSlot && (g_sinks[i].fn == nullptr) && (g_sinkActive[i] == 0U)) {
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
  std::unique_lock<std::mutex> lock(g_sinkMutex);
  for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
    if ((g_sinks[i].fn == fn) && (g_sinks[i].userData == userData)) {
      // Clearing the slot stops new dispatches from picking the sink up;
      // waiting for the slot to fall quiescent covers the dispatches that
      // already snapshotted it, so the owner may release userData as soon as
      // this returns.
      g_sinks[i] = SinkSlot{};
      wait_for_slots_quiescent(lock, i, i + 1U);
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
