// Declares the editor Console's bounded log capture, filtering, duplicate
// collapse, and best-effort source/entity navigation metadata (issue #155).
// Panel-draw-code exempt: every symbol here is testable without ImGui.

#pragma once

#include "engine/core/logging.h"
#include "engine/runtime/world.h"

#include <cstddef>
#include <cstdint>

namespace engine::editor {

/// Distinguishes engine-side diagnostics from script (Lua) diagnostics; the
/// panel filters and colors entries by this in addition to severity.
enum class ConsoleSourceCategory : std::uint8_t { Engine, Script };

/// What kind of file reference, if any, was parsed out of an entry's
/// message text. Parsing is a best-effort heuristic over already-formatted
/// diagnostic strings (there is no structured error-context channel today),
/// so callers must treat a match as a navigation hint, not a guarantee.
enum class ConsoleReferenceKind : std::uint8_t { None, ScriptLocation, AssetPath };

/// Hard bound on captured entries; the ring drops the oldest entry once
/// full rather than growing (issue #155 storage-bound requirement). At
/// worst case (every field's static size) this is a few MB, acceptable for
/// an editor-only in-memory tool, never written to disk or streamed.
constexpr std::size_t kMaxConsoleEntries = 2048U;

/// Fixed capacity for message text; longer messages (chiefly Lua
/// tracebacks) are truncated with a trailing ellipsis marker. This is
/// presentation truncation of a diagnostic string, not truncation of
/// authored user data, so the atomic-write contract does not apply.
constexpr std::size_t kConsoleMessageCapacity = 512U;
constexpr std::size_t kConsoleChannelCapacity = 32U;
/// Fixed capacity for a parsed script or asset path reference.
constexpr std::size_t kConsolePathCapacity = 192U;

/// Sentinel meaning "no entity index was parsed from this entry's message."
constexpr std::uint32_t kConsoleNoEntityHint = 0xFFFFFFFFU;

/// One captured, storage-bounded diagnostic line plus its collapse and
/// navigation metadata. Every field is fixed-size: no owned heap memory.
struct ConsoleEntry final {
  core::LogLevel level = core::LogLevel::Info;
  ConsoleSourceCategory category = ConsoleSourceCategory::Engine;
  char channel[kConsoleChannelCapacity] = {};
  char message[kConsoleMessageCapacity] = {};
  /// True when the message text was longer than the buffer and was
  /// truncated with an ellipsis marker (diagnostic presentation only).
  bool truncated = false;
  /// Milliseconds since this capture session began (steady clock); always
  /// available. Frame index is best-effort and 0 when never published.
  std::uint64_t captureTimeMs = 0U;
  std::uint32_t frameIndex = 0U;
  /// Monotonic ingest sequence number, used to order entries and to bound
  /// "current session" filtering to entries logged after the last
  /// begin-session marker (see console_capture_begin_session).
  std::uint64_t sequence = 0U;
  /// Number of times this exact (level, channel, message) was logged back
  /// to back; incremented in place instead of pushing a new ring slot.
  std::uint32_t repeatCount = 1U;

  ConsoleReferenceKind referenceKind = ConsoleReferenceKind::None;
  char referencePath[kConsolePathCapacity] = {};
  /// 1-based source line for ConsoleReferenceKind::ScriptLocation; -1 when
  /// no line number was found.
  int referenceLine = -1;

  /// Entity index parsed from the message text, or kConsoleNoEntityHint.
  /// A hint only: resolving it to a live Entity requires re-checking the
  /// attached World at click time (see console_capture_resolve_entity_hint).
  std::uint32_t entityIndexHint = kConsoleNoEntityHint;
};

/// Installs the capture sink with core logging (idempotent) and resets all
/// state to empty. Call once during editor startup; safe to call again to
/// force a clean reset (e.g. in tests).
void console_capture_initialize() noexcept;
/// Uninstalls the capture sink and clears all state.
void console_capture_shutdown() noexcept;

/// Clears every captured entry and unseen/badge counters without touching
/// sink registration (the "Clear" button's production path).
void console_capture_clear() noexcept;

/// Marks a new navigation boundary: entries logged after this call belong
/// to the "current session" filter scope (see ConsoleFilter::sessionOnly).
/// The editor calls this when Play starts so "current session" reads as
/// "since I hit Play."
void console_capture_begin_session() noexcept;

/// Number of entries currently retained (<= kMaxConsoleEntries).
std::size_t console_capture_entry_count() noexcept;
/// Copies entry `index` (0 = oldest retained) into *out. Returns false for
/// an out-of-range index or a null out pointer.
bool console_capture_get_entry(std::size_t index, ConsoleEntry *out) noexcept;

/// Total entries ever ingested (pre-collapse, pre-overflow-drop); lets
/// tests and the UI distinguish "ring wrapped" from "nothing logged yet."
std::uint64_t console_capture_total_ingested() noexcept;

/// Badge counters for surfacing severity while the panel is closed
/// (issue #155's non-spamming status indicator). Counts entries at or
/// above the given level ingested since the last console_capture_mark_seen
/// call (each repeat of a collapsed entry still increments this once).
std::uint32_t console_capture_unseen_error_count() noexcept;
std::uint32_t console_capture_unseen_warning_count() noexcept;
/// Resets both unseen counters to zero (call when the panel becomes
/// visible/focused).
void console_capture_mark_seen() noexcept;

/// Filter/search state the Console panel edits and applies at draw time;
/// kept separate from ConsoleEntry so filtering never mutates captured
/// data. Every field defaults to "show everything."
struct ConsoleFilter final {
  bool showTrace = true;
  bool showInfo = true;
  bool showWarning = true;
  bool showError = true;
  bool showFatal = true;
  /// Case-insensitive substring match against channel and message; empty
  /// string matches everything.
  char searchText[128] = {};
  /// Exact channel match; empty string matches every channel.
  char channelFilter[kConsoleChannelCapacity] = {};
  /// When true, only entries with sequence >= the last begin-session
  /// marker are shown (see console_capture_begin_session).
  bool sessionOnly = false;
};

/// True when `entry` passes every active clause of `filter`. Pure function
/// over caller-owned data — the production path the panel and tests share.
bool console_filter_matches(const ConsoleFilter &filter,
                            const ConsoleEntry &entry) noexcept;

/// Resolves an entity-index navigation hint against the given world: an
/// entity is "safe" to select only when the world pointer is non-null and
/// the index still names a currently alive entity (index reuse across a
/// scene load or entity destruction must not silently select the wrong
/// object). Returns runtime::kInvalidEntity when unsafe or unresolved.
runtime::Entity
console_capture_resolve_entity_hint(std::uint32_t entityIndexHint,
                                    const runtime::World *world) noexcept;

} // namespace engine::editor
