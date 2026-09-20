// Implements the editor Console's bounded log capture, filtering, duplicate
// collapse, and the navigation metadata each entry takes from its
// diagnostic record.

#include "editor_console_capture.h"

#include "engine/core/diagnostic.h"
#include "engine/core/fixed_ring.h"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>

namespace engine::editor {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_captureMutex{};
/// Retained entries, oldest first; a full ring drops its oldest entry so
/// the newest diagnostics are always the ones kept. Guarded by
/// g_captureMutex.
engine::core::FixedRing<ConsoleEntry, kMaxConsoleEntries> g_ring{};
std::uint64_t g_nextSequence = 1U;
std::uint64_t g_totalIngested = 0U;
std::uint64_t g_sessionMarkerSeq = 0U;
// The capture epoch, read lock-free by the sink from any logging thread
// and written by console_capture_initialize under the mutex; atomic so
// the two never race. Any value the sink reads is a valid epoch:
// a re-initialize only re-bases later timestamps.
std::atomic<Clock::rep> g_captureStartTicks{0};
bool g_sinkRegistered = false;

std::atomic<std::uint32_t> g_unseenErrors{0U};
std::atomic<std::uint32_t> g_unseenWarnings{0U};

/// Copies `src` into a fixed buffer, truncating with a trailing "..." marker
/// when it does not fit; this is presentation truncation of an already-
/// formatted diagnostic string (documented capacity, never authored data).
void copy_truncated(char *dst, std::size_t dstCapacity, const char *src,
                    bool *outTruncated) noexcept {
  if (outTruncated != nullptr) {
    *outTruncated = false;
  }
  if ((dst == nullptr) || (dstCapacity == 0U)) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  const std::size_t srcLen = std::strlen(src);
  if (srcLen < dstCapacity) {
    std::memcpy(dst, src, srcLen + 1U);
    return;
  }
  constexpr char kEllipsis[] = "...";
  constexpr std::size_t kEllipsisLen = sizeof(kEllipsis) - 1U;
  const std::size_t keep =
      (dstCapacity > kEllipsisLen) ? (dstCapacity - 1U - kEllipsisLen) : 0U;
  std::memcpy(dst, src, keep);
  std::memcpy(dst + keep, kEllipsis, kEllipsisLen);
  dst[keep + kEllipsisLen] = '\0';
  if (outTruncated != nullptr) {
    *outTruncated = true;
  }
}

/// True when `channel` is the scripting channel; the name is the enum's,
/// so a call site cannot spell its way out of the Script filter.
ConsoleSourceCategory classify_category(const char *channel) noexcept {
  if ((channel != nullptr) &&
      (std::strcmp(channel,
                   core::log_channel_name(core::LogChannel::Scripting)) == 0)) {
    return ConsoleSourceCategory::Script;
  }
  return ConsoleSourceCategory::Engine;
}

/// Appends `candidate` to the ring under g_captureMutex, collapsing into the
/// immediately preceding entry when it is an exact repeat (bounds the
/// collapse check to O(1) — no scan across the whole ring). Updates the
/// unseen badge counters. Called with the lock already held.
void ingest_locked(ConsoleEntry candidate) noexcept {
  ++g_totalIngested;
  candidate.sequence = g_nextSequence++;

  ConsoleEntry *last = g_ring.back();
  if ((last != nullptr) && (last->level == candidate.level) &&
      (std::strcmp(last->channel, candidate.channel) == 0) &&
      (std::strcmp(last->message, candidate.message) == 0)) {
    ++last->repeatCount;
    last->sequence = candidate.sequence;
    last->captureTimeMs = candidate.captureTimeMs;
    last->frameIndex = candidate.frameIndex;
    return; // collapsed; no new slot, no additional badge increment
  }

  static_cast<void>(g_ring.push_overwrite(candidate));

  if (candidate.level >= core::LogLevel::Error) {
    g_unseenErrors.fetch_add(1U, std::memory_order_relaxed);
  } else if (candidate.level == core::LogLevel::Warning) {
    g_unseenWarnings.fetch_add(1U, std::memory_order_relaxed);
  }
}

/// The registered core diagnostic sink. The entry is built before the
/// lock is taken so the critical section stays a fixed-size copy/compare,
/// matching the lock-light contract.
void console_capture_sink(const core::Diagnostic &record,
                          void * /*userData*/) noexcept {
  ConsoleEntry candidate{};
  candidate.level = record.level;
  candidate.category = classify_category(record.channel);
  copy_truncated(candidate.channel, sizeof(candidate.channel), record.channel,
                nullptr);
  copy_truncated(candidate.message, sizeof(candidate.message), record.message,
                &candidate.truncated);
  candidate.frameIndex = record.frame;
  const Clock::time_point captureStart{Clock::duration(
      g_captureStartTicks.load(std::memory_order_relaxed))};
  candidate.captureTimeMs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::now() - captureStart)
          .count());

  static_assert(sizeof(candidate.referencePath) >= sizeof(record.path),
                "an entry holds a whole record path");
  if (record.path[0] != '\0') {
    std::memcpy(candidate.referencePath, record.path, sizeof(record.path));
    candidate.referenceLine = record.line;
    candidate.referenceKind = (record.line >= 0)
                                  ? ConsoleReferenceKind::ScriptLocation
                                  : ConsoleReferenceKind::AssetPath;
  }
  candidate.entityPersistentId = record.entityPersistentId;

  std::lock_guard<std::mutex> lock(g_captureMutex);
  ingest_locked(candidate);
}

/// Resets every piece of capture state to empty; shared by initialize,
/// shutdown, and Clear so the three can never drift out of sync with each
/// other about which counters "empty" resets. Called with the lock held.
///
/// The ring is ~1.5MB (2048 entries of 784 bytes) and this runs on the
/// caller's thread, which on Windows has a ~1MB default stack: the ring's
/// clear() resets one element at a time and never materializes a
/// whole-array temporary, so this stays safe on that stack whatever the
/// compiler elides.
void reset_state_locked() noexcept {
  g_ring.clear();
  g_nextSequence = 1U;
  g_totalIngested = 0U;
  g_sessionMarkerSeq = 0U;
  g_unseenErrors.store(0U, std::memory_order_relaxed);
  g_unseenWarnings.store(0U, std::memory_order_relaxed);
}

} // namespace

void console_capture_initialize() noexcept {
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    reset_state_locked();
    g_captureStartTicks.store(Clock::now().time_since_epoch().count(),
                              std::memory_order_relaxed);
  }

  if (!g_sinkRegistered) {
    g_sinkRegistered =
        core::log_register_diagnostic_sink(&console_capture_sink, nullptr);
  }
}

void console_capture_shutdown() noexcept {
  if (g_sinkRegistered) {
    core::log_unregister_diagnostic_sink(&console_capture_sink, nullptr);
    g_sinkRegistered = false;
  }
  std::lock_guard<std::mutex> lock(g_captureMutex);
  reset_state_locked();
}

void console_capture_clear() noexcept {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  reset_state_locked();
}

void console_capture_begin_session() noexcept {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  g_sessionMarkerSeq = g_nextSequence;
}

std::size_t console_capture_entry_count() noexcept {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  return g_ring.size();
}

bool console_capture_get_entry(std::size_t index, ConsoleEntry *out) noexcept {
  if (out == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_captureMutex);
  const ConsoleEntry *entry = g_ring.at(index);
  if (entry == nullptr) {
    return false;
  }
  *out = *entry;
  return true;
}

std::uint64_t console_capture_total_ingested() noexcept {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  return g_totalIngested;
}

std::uint32_t console_capture_unseen_error_count() noexcept {
  return g_unseenErrors.load(std::memory_order_relaxed);
}

std::uint32_t console_capture_unseen_warning_count() noexcept {
  return g_unseenWarnings.load(std::memory_order_relaxed);
}

void console_capture_mark_seen() noexcept {
  g_unseenErrors.store(0U, std::memory_order_relaxed);
  g_unseenWarnings.store(0U, std::memory_order_relaxed);
}

namespace {

/// Case-insensitive substring test; std::string-free to match the fixed-
/// buffer style of the rest of this module.
bool contains_ci(const char *haystack, const char *needle) noexcept {
  if ((needle == nullptr) || (needle[0] == '\0')) {
    return true;
  }
  if (haystack == nullptr) {
    return false;
  }
  const std::size_t needleLen = std::strlen(needle);
  const std::size_t haystackLen = std::strlen(haystack);
  if (needleLen > haystackLen) {
    return false;
  }
  for (std::size_t start = 0U; start + needleLen <= haystackLen; ++start) {
    bool matches = true;
    for (std::size_t i = 0U; i < needleLen; ++i) {
      if (std::tolower(static_cast<unsigned char>(haystack[start + i])) !=
          std::tolower(static_cast<unsigned char>(needle[i]))) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
}

} // namespace

bool console_filter_matches(const ConsoleFilter &filter,
                            const ConsoleEntry &entry) noexcept {
  switch (entry.level) {
  case core::LogLevel::Trace:
    if (!filter.showTrace) {
      return false;
    }
    break;
  case core::LogLevel::Info:
    if (!filter.showInfo) {
      return false;
    }
    break;
  case core::LogLevel::Warning:
    if (!filter.showWarning) {
      return false;
    }
    break;
  case core::LogLevel::Error:
    if (!filter.showError) {
      return false;
    }
    break;
  case core::LogLevel::Fatal:
    if (!filter.showFatal) {
      return false;
    }
    break;
  default:
    break;
  }

  if ((filter.channelFilter[0] != '\0') &&
     (std::strcmp(filter.channelFilter, entry.channel) != 0)) {
    return false;
  }

  if (filter.sessionOnly && (entry.sequence < g_sessionMarkerSeq)) {
    return false;
  }

  if (filter.searchText[0] != '\0') {
    if (!contains_ci(entry.message, filter.searchText) &&
       !contains_ci(entry.channel, filter.searchText)) {
      return false;
    }
  }

  return true;
}

runtime::Entity
console_capture_resolve_entity(runtime::PersistentId entityPersistentId,
                               const runtime::World *world) noexcept {
  if ((world == nullptr) ||
      (entityPersistentId == runtime::kInvalidPersistentId)) {
    return runtime::kInvalidEntity;
  }
  // The persistent id names the authored entity, so a reload that
  // re-creates it under a new index still resolves, and a destroyed one
  // does not.
  return world->find_entity_by_persistent_id(entityPersistentId);
}

} // namespace engine::editor
