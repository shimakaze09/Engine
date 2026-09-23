// Implements logging behavior for the Engine core engine: the text line,
// the structured record every line also carries, and the shared sink
// table both kinds of sink live in.

#include "engine/core/logging.h"

#include "engine/core/diagnostic.h"
#include "engine/core/job_system.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>


namespace engine::core {

namespace {

std::atomic<bool> g_loggingInitialized{false};
std::atomic<std::uint32_t> g_frameIndex{0U};

/// One fixed sink table slot; unused slots have fn == nullptr. A retiring
/// slot keeps its (fn, userData) pair matchable while its removal drains:
/// dispatches skip it, registration treats it as occupied, and any remover
/// that matches it — a second concurrent remover of the same pair, or one
/// racing shutdown's table-wide retire — waits on the same quiescence
/// instead of returning while a dispatch may still be inside the sink.
struct SinkSlot final {
  LogSinkFn fn = nullptr;
  DiagnosticSinkFn recordFn = nullptr;
  void *userData = nullptr;
  bool retiring = false;

  /// True when the slot holds either kind of sink.
  bool occupied() const noexcept {
    return (fn != nullptr) || (recordFn != nullptr);
  }
  /// True when the slot holds exactly this registration.
  bool matches(LogSinkFn textFn, DiagnosticSinkFn diagnosticFn,
               void *data) const noexcept {
    return (fn == textFn) && (recordFn == diagnosticFn) && (userData == data);
  }
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
                       const char *message,
                       const Diagnostic &record) noexcept {
  std::array<SinkSlot, kMaxLogSinks> snapshot{};
  {
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    snapshot = g_sinks;
    for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
      // A retiring slot is no longer dispatched: its removal is already
      // draining, and a new claim here would extend the wait it is
      // draining toward. Blanked in the snapshot so the call loop below
      // cannot pick it up.
      if (snapshot[i].retiring) {
        snapshot[i] = SinkSlot{};
        continue;
      }
      if (snapshot[i].occupied()) {
        ++g_sinkActive[i];
        ++t_sinkActiveOnThread[i];
      }
    }
  }
  for (const SinkSlot &slot : snapshot) {
    if (slot.fn != nullptr) {
      slot.fn(level, channel, message, slot.userData);
    } else if (slot.recordFn != nullptr) {
      slot.recordFn(record, slot.userData);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
      if (snapshot[i].occupied()) {
        --g_sinkActive[i];
        --t_sinkActiveOnThread[i];
      }
    }
  }
  // Notified outside the lock; the counts above drop under it, so a waiter
  // holding the lock cannot miss this wakeup.
  g_sinkQuiescent.notify_all();
}

/// Copies `text` into a fixed field, cutting it to fit.
void copy_field(char *out, std::size_t capacity, const char *text) noexcept {
  if (capacity == 0U) {
    return;
  }
  if (text == nullptr) {
    out[0] = '\0';
    return;
  }
  std::snprintf(out, capacity, "%s", text);
}

/// Prints the text line and delivers it to every sink; `text` is the
/// full message (log_message prints all of it, however long) and
/// `record` carries the structured fields.
void emit(LogLevel level, const char *channel, const char *text,
          Diagnostic record) noexcept {
  if (!g_loggingInitialized.load(std::memory_order_acquire)) {
    return;
  }

  record.frame = g_frameIndex.load(std::memory_order_relaxed);
  record.thread = current_thread_index();

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
             channel, text);

  // Sinks run even for Fatal so an editor-side capture still records the
  // message that is about to abort the process (the Fatal-only-abort
  // contract governs process exit, not diagnostic capture).
  dispatch_to_sinks(level, channel, text, record);

  // Error and Fatal flush. When stdout is a pipe or a file it is fully
  // buffered, so whatever is still in the buffer dies with the process --
  // and the lines immediately before a crash are the ones worth having.
  // Trace, Info and Warning stay buffered: they are the bulk of the
  // output, and flushing each one costs a syscall per line.
  if ((level == LogLevel::Error) || (level == LogLevel::Fatal)) {
    std::fflush(stdout);
  }
  if (level == LogLevel::Fatal) {
    std::abort();
  }
}

/// Registers one sink of either kind into a free slot.
bool register_slot(LogSinkFn fn, DiagnosticSinkFn recordFn,
                   void *userData) noexcept {
  if ((fn == nullptr) == (recordFn == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_sinkMutex);
  std::size_t freeIndex = kMaxLogSinks;
  for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
    if (g_sinks[i].matches(fn, recordFn, userData)) {
      return false;
    }
    if ((freeIndex == kMaxLogSinks) && !g_sinks[i].occupied() &&
        (g_sinkActive[i] == 0U)) {
      freeIndex = i;
    }
  }
  if (freeIndex == kMaxLogSinks) {
    return false;
  }
  g_sinks[freeIndex] = SinkSlot{fn, recordFn, userData, false};
  return true;
}

/// Unregisters one sink of either kind, waiting out any dispatch inside it.
void unregister_slot(LogSinkFn fn, DiagnosticSinkFn recordFn,
                     void *userData) noexcept {
  std::unique_lock<std::mutex> lock(g_sinkMutex);
  for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
    if (g_sinks[i].matches(fn, recordFn, userData)) {
      g_sinks[i].retiring = true;
      wait_for_slots_quiescent(lock, i, i + 1U);
      if (g_sinks[i].matches(fn, recordFn, userData) && g_sinks[i].retiring) {
        g_sinks[i] = SinkSlot{};
      }
      return;
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
  // Drop any sink its owner failed to unregister so a dead sink is
  // never dispatched to after a later re-initialization. Teardown owes the
  // same lifetime barrier as unregister: it returns only once no dispatch is
  // still inside one of the sinks it just dropped. Slots are retired, not
  // cleared, until the drain completes, so an unregister racing this
  // teardown still matches its pair and waits on the same quiescence
  // instead of finding an already-empty table and returning early.
  std::unique_lock<std::mutex> lock(g_sinkMutex);
  for (std::size_t i = 0U; i < kMaxLogSinks; ++i) {
    if (g_sinks[i].occupied()) {
      g_sinks[i].retiring = true;
    }
  }
  wait_for_slots_quiescent(lock, 0U, kMaxLogSinks);
  g_sinks = {};
}

const char *log_channel_name(LogChannel channel) noexcept {
  switch (channel) {
  case LogChannel::Engine:
    return "engine";
  case LogChannel::Runtime:
    return "runtime";
  case LogChannel::World:
    return "world";
  case LogChannel::Renderer:
    return "renderer";
  case LogChannel::RenderDevice:
    return "render_device";
  case LogChannel::RenderPrep:
    return "render_prep";
  case LogChannel::Shader:
    return "shader";
  case LogChannel::Shadow:
    return "shadow";
  case LogChannel::ShadowMap:
    return "shadow_map";
  case LogChannel::PassResources:
    return "pass_resources";
  case LogChannel::Bgfx:
    return "bgfx";
  case LogChannel::Scripting:
    return "scripting";
  case LogChannel::Dap:
    return "dap";
  case LogChannel::Editor:
    return "editor";
  case LogChannel::Assets:
    return "assets";
  case LogChannel::AssetStreaming:
    return "asset_streaming";
  case LogChannel::Streaming:
    return "streaming";
  case LogChannel::Save:
    return "save";
  case LogChannel::Prefab:
    return "prefab";
  case LogChannel::Audio:
    return "audio";
  case LogChannel::Physics:
    return "physics";
  case LogChannel::Animation:
    return "animation";
  case LogChannel::EntityPool:
    return "entity_pool";
  case LogChannel::Jobs:
    return "jobs";
  case LogChannel::Slice:
    return "slice";
  }
  return "engine";
}

void log_message(LogLevel level, LogChannel channel,
                 const char *message) noexcept {
  log_message(level, log_channel_name(channel), message);
}

void log_message(LogLevel level,
                 const char *channel,
                 const char *message) noexcept {
  emit(level, channel, (message != nullptr) ? message : "",
       make_diagnostic(level, channel, message));
}

Diagnostic make_diagnostic(LogLevel level, const char *channel,
                           const char *message) noexcept {
  Diagnostic record{};
  record.level = level;
  copy_field(record.channel, sizeof(record.channel), channel);
  if (message == nullptr) {
    return record;
  }
  const std::size_t length = std::strlen(message);
  if (length < sizeof(record.message)) {
    std::memcpy(record.message, message, length + 1U);
  } else {
    constexpr char kCut[] = "...";
    const std::size_t keep = sizeof(record.message) - sizeof(kCut);
    std::memcpy(record.message, message, keep);
    std::memcpy(record.message + keep, kCut, sizeof(kCut));
  }
  return record;
}

Diagnostic make_diagnostic(LogLevel level, LogChannel channel,
                           const char *message) noexcept {
  return make_diagnostic(level, log_channel_name(channel), message);
}

void diagnostic_set_path(Diagnostic *diagnostic, const char *path) noexcept {
  if (diagnostic != nullptr) {
    copy_field(diagnostic->path, sizeof(diagnostic->path), path);
  }
}

void log_diagnostic(const Diagnostic &diagnostic) noexcept {
  emit(diagnostic.level, diagnostic.channel, diagnostic.message, diagnostic);
}

void log_path_diagnostic(LogLevel level, const char *channel,
                         const char *path, const char *reason) noexcept {
  char text[Diagnostic::kMaxMessage] = {};
  std::snprintf(text, sizeof(text), "%s: %s",
                (path != nullptr) ? path : "(null)",
                (reason != nullptr) ? reason : "unknown error");
  Diagnostic record = make_diagnostic(level, channel, text);
  diagnostic_set_path(&record, path);
  log_diagnostic(record);
}

void log_path_diagnostic(LogLevel level, LogChannel channel, const char *path,
                         const char *reason) noexcept {
  log_path_diagnostic(level, log_channel_name(channel), path, reason);
}

void log_set_frame_index(std::uint32_t frameIndex) noexcept {
  g_frameIndex.store(frameIndex, std::memory_order_relaxed);
}

std::uint32_t log_current_frame_index() noexcept {
  return g_frameIndex.load(std::memory_order_relaxed);
}

bool log_register_sink(LogSinkFn fn, void *userData) noexcept {
  return register_slot(fn, nullptr, userData);
}

void log_unregister_sink(LogSinkFn fn, void *userData) noexcept {
  unregister_slot(fn, nullptr, userData);
}

bool log_register_diagnostic_sink(DiagnosticSinkFn fn,
                                  void *userData) noexcept {
  return register_slot(nullptr, fn, userData);
}

void log_unregister_diagnostic_sink(DiagnosticSinkFn fn,
                                    void *userData) noexcept {
  unregister_slot(nullptr, fn, userData);
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
