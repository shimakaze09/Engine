// Implements the editor Console's bounded log capture, filtering, duplicate
// collapse, and best-effort source/entity navigation metadata (issue #155).

#include "editor_console_capture.h"

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
std::array<ConsoleEntry, kMaxConsoleEntries> g_ring{};
std::size_t g_head = 0U;  // index of oldest retained entry
std::size_t g_count = 0U; // entries currently retained (<= capacity)
std::uint64_t g_nextSequence = 1U;
std::uint64_t g_totalIngested = 0U;
std::uint64_t g_sessionMarkerSeq = 0U;
Clock::time_point g_captureStart{};
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

/// True when `c` may appear inside a relative VFS-jailed asset/script path
/// (issue #83 jail rules: relative, forward slashes, no drive letters).
bool is_path_char(char c) noexcept {
  return (std::isalnum(static_cast<unsigned char>(c)) != 0) || (c == '/') ||
        (c == '_') || (c == '-') || (c == '.');
}

/// Scans `message` for the standard Lua chunk-error shape "<path>:<line>:"
/// (produced by Lua itself for every loaded chunk, surfaced verbatim by
/// binding_util's log_lua_error). Returns true and fills path/line on a
/// match; a miss leaves outPath empty and outLine at -1.
bool parse_script_location(const char *message, char *outPath,
                           std::size_t outPathCapacity,
                           int *outLine) noexcept {
  outPath[0] = '\0';
  *outLine = -1;
  if (message == nullptr) {
    return false;
  }

  constexpr char kMarker[] = ".lua:";
  const char *hit = std::strstr(message, kMarker);
  if (hit == nullptr) {
    return false;
  }
  const char *extEnd = hit + 4; // position of ':' right after ".lua"

  const char *start = hit;
  while ((start > message) && is_path_char(*(start - 1))) {
    --start;
  }
  if (start == extEnd) {
    return false; // no path characters before the extension
  }

  const char *digits = extEnd + 1;
  if ((*digits < '0') || (*digits > '9')) {
    return false;
  }
  long line = 0;
  const char *cursor = digits;
  while ((*cursor >= '0') && (*cursor <= '9')) {
    line = (line * 10) + (*cursor - '0');
    ++cursor;
    if (line > 1000000000L) {
      break; // guard against a corrupt/adversarial digit run
    }
  }

  const std::size_t pathLen = static_cast<std::size_t>(extEnd - start);
  const std::size_t copyLen =
      (pathLen < outPathCapacity) ? pathLen : (outPathCapacity - 1U);
  std::memcpy(outPath, start, copyLen);
  outPath[copyLen] = '\0';
  *outLine = static_cast<int>(line);
  return true;
}

/// Best-effort scan for a whitespace-delimited token that looks like a
/// relative asset path (contains '/' and a short trailing extension).
/// Heuristic over unstructured message text — see the class comment on
/// ConsoleReferenceKind for why this can miss legitimate references.
bool parse_asset_path(const char *message, char *outPath,
                      std::size_t outPathCapacity) noexcept {
  outPath[0] = '\0';
  if (message == nullptr) {
    return false;
  }

  const char *cursor = message;
  while (*cursor != '\0') {
    while ((*cursor != '\0') &&
          ((std::isspace(static_cast<unsigned char>(*cursor)) != 0) ||
           (*cursor == '(') || (*cursor == ')') || (*cursor == '\'') ||
           (*cursor == '"'))) {
      ++cursor;
    }
    const char *tokenStart = cursor;
    while (is_path_char(*cursor)) {
      ++cursor;
    }
    const char *tokenEnd = cursor;
    while ((*cursor != '\0') &&
          (std::isspace(static_cast<unsigned char>(*cursor)) == 0) &&
          (*cursor != '(') && (*cursor != ')')) {
      ++cursor; // skip trailing punctuation (e.g. a sentence's ':' or ',')
    }

    while ((tokenEnd > tokenStart) &&
          ((*(tokenEnd - 1) == '.') || (*(tokenEnd - 1) == ':'))) {
      --tokenEnd; // trim trailing punctuation glued onto the path token
    }

    const std::size_t tokenLen = static_cast<std::size_t>(tokenEnd - tokenStart);
    if (tokenLen >= 3U) {
      bool hasSlash = false;
      bool hasDotWithExt = false;
      for (std::size_t i = 0U; i < tokenLen; ++i) {
        if (tokenStart[i] == '/') {
          hasSlash = true;
        }
        if ((tokenStart[i] == '.') && (tokenLen - i >= 2U) &&
            (tokenLen - i <= 6U)) {
          hasDotWithExt = true;
        }
      }
      if (hasSlash && hasDotWithExt) {
        const std::size_t copyLen =
            (tokenLen < outPathCapacity) ? tokenLen : (outPathCapacity - 1U);
        std::memcpy(outPath, tokenStart, copyLen);
        outPath[copyLen] = '\0';
        return true;
      }
    }

    if (*cursor == '\0') {
      break;
    }
  }
  return false;
}

/// Best-effort scan for "entity <digits>" (case-insensitive), the shape
/// used by cheat/spawn diagnostics today. A hint only — resolved against
/// the live World at click time, never trusted as an alive guarantee.
std::uint32_t parse_entity_index_hint(const char *message) noexcept {
  if (message == nullptr) {
    return kConsoleNoEntityHint;
  }
  constexpr char kWord[] = "entity";
  constexpr std::size_t kWordLen = sizeof(kWord) - 1U;
  const std::size_t len = std::strlen(message);
  for (std::size_t i = 0U; i + kWordLen <= len; ++i) {
    bool matches = true;
    for (std::size_t j = 0U; j < kWordLen; ++j) {
      if (std::tolower(static_cast<unsigned char>(message[i + j])) !=
          kWord[j]) {
        matches = false;
        break;
      }
    }
    if (!matches) {
      continue;
    }
    std::size_t cursor = i + kWordLen;
    while ((cursor < len) &&
          (std::isspace(static_cast<unsigned char>(message[cursor])) != 0)) {
      ++cursor;
    }
    // Skip an optional connector such as "entity index=7" or "entity #7".
    while ((cursor < len) &&
          ((message[cursor] == '=') || (message[cursor] == '#') ||
           (message[cursor] == ':'))) {
      ++cursor;
    }
    while ((cursor < len) &&
          (std::isspace(static_cast<unsigned char>(message[cursor])) != 0)) {
      ++cursor;
    }
    if ((cursor >= len) || (message[cursor] < '0') ||
       (message[cursor] > '9')) {
      continue;
    }
    std::uint64_t value = 0U;
    while ((cursor < len) && (message[cursor] >= '0') &&
          (message[cursor] <= '9')) {
      value = (value * 10U) + static_cast<std::uint64_t>(message[cursor] - '0');
      ++cursor;
      if (value >= kConsoleNoEntityHint) {
        return kConsoleNoEntityHint; // overflow guard; not a plausible index
      }
    }
    return static_cast<std::uint32_t>(value);
  }
  return kConsoleNoEntityHint;
}

/// True when `channel` names a scripting diagnostic (see log_lua_error and
/// its callers, all of which log under the "scripting" channel).
ConsoleSourceCategory classify_category(const char *channel) noexcept {
  if ((channel != nullptr) && (std::strcmp(channel, "scripting") == 0)) {
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

  if (g_count > 0U) {
    const std::size_t lastSlot = (g_head + g_count - 1U) % kMaxConsoleEntries;
    ConsoleEntry &last = g_ring[lastSlot];
    if ((last.level == candidate.level) &&
       (std::strcmp(last.channel, candidate.channel) == 0) &&
       (std::strcmp(last.message, candidate.message) == 0)) {
      ++last.repeatCount;
      last.sequence = candidate.sequence;
      last.captureTimeMs = candidate.captureTimeMs;
      last.frameIndex = candidate.frameIndex;
      return; // collapsed; no new slot, no additional badge increment
    }
  }

  const std::size_t writeSlot = (g_head + g_count) % kMaxConsoleEntries;
  g_ring[writeSlot] = candidate;
  if (g_count < kMaxConsoleEntries) {
    ++g_count;
  } else {
    g_head = (g_head + 1U) % kMaxConsoleEntries; // drop oldest, ring is full
  }

  if (candidate.level >= core::LogLevel::Error) {
    g_unseenErrors.fetch_add(1U, std::memory_order_relaxed);
  } else if (candidate.level == core::LogLevel::Warning) {
    g_unseenWarnings.fetch_add(1U, std::memory_order_relaxed);
  }
}

/// The registered core logging sink (issue #155's capture hook). All
/// parsing work happens before the lock is taken so the critical section
/// stays a fixed-size copy/compare, matching the lock-light contract.
void console_capture_sink(core::LogLevel level, const char *channel,
                          const char *message, void * /*userData*/) noexcept {
  ConsoleEntry candidate{};
  candidate.level = level;
  candidate.category = classify_category(channel);
  copy_truncated(candidate.channel, sizeof(candidate.channel), channel,
                nullptr);
  copy_truncated(candidate.message, sizeof(candidate.message), message,
                &candidate.truncated);
  candidate.frameIndex = core::log_current_frame_index();
  candidate.captureTimeMs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::now() - g_captureStart)
          .count());

  char scriptPath[kConsolePathCapacity] = {};
  int scriptLine = -1;
  if (parse_script_location(candidate.message, scriptPath,
                            sizeof(scriptPath), &scriptLine)) {
    candidate.referenceKind = ConsoleReferenceKind::ScriptLocation;
    std::memcpy(candidate.referencePath, scriptPath, sizeof(scriptPath));
    candidate.referenceLine = scriptLine;
  } else {
    char assetPath[kConsolePathCapacity] = {};
    if (parse_asset_path(candidate.message, assetPath, sizeof(assetPath))) {
      candidate.referenceKind = ConsoleReferenceKind::AssetPath;
      std::memcpy(candidate.referencePath, assetPath, sizeof(assetPath));
    }
  }
  candidate.entityIndexHint = parse_entity_index_hint(candidate.message);

  std::lock_guard<std::mutex> lock(g_captureMutex);
  ingest_locked(candidate);
}

/// Resets every piece of capture state to empty; shared by initialize,
/// shutdown, and Clear so the three can never drift out of sync with each
/// other about which counters "empty" resets. Called with the lock held.
void reset_state_locked() noexcept {
  g_ring = {};
  g_head = 0U;
  g_count = 0U;
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
    g_captureStart = Clock::now();
  }

  if (!g_sinkRegistered) {
    g_sinkRegistered =
        core::log_register_sink(&console_capture_sink, nullptr);
  }
}

void console_capture_shutdown() noexcept {
  if (g_sinkRegistered) {
    core::log_unregister_sink(&console_capture_sink, nullptr);
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
  return g_count;
}

bool console_capture_get_entry(std::size_t index, ConsoleEntry *out) noexcept {
  if (out == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_captureMutex);
  if (index >= g_count) {
    return false;
  }
  *out = g_ring[(g_head + index) % kMaxConsoleEntries];
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
console_capture_resolve_entity_hint(std::uint32_t entityIndexHint,
                                    const runtime::World *world) noexcept {
  if ((world == nullptr) || (entityIndexHint == kConsoleNoEntityHint)) {
    return runtime::kInvalidEntity;
  }
  // find_entity_by_index already refuses a dead/unknown index (world_
  // lifecycle.cpp), so a returned handle is safe to select immediately.
  return world->find_entity_by_index(entityIndexHint);
}

} // namespace engine::editor
