// Declares the editor Console's bounded log capture, filtering, duplicate
// collapse, and the source/entity navigation metadata each entry carries
// from its diagnostic record.
// Panel-draw-code exempt: every symbol here is testable without ImGui.

#pragma once

#include "engine/core/logging.h"
#include "engine/runtime/world.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::editor {

/// Distinguishes engine-side diagnostics from script (Lua) diagnostics; the
/// panel filters and colors entries by this in addition to severity.
enum class ConsoleSourceCategory : std::uint8_t { Engine, Script };

/// What kind of file reference, if any, the entry's diagnostic record
/// carried: a script location when the producer named a path and a line,
/// an asset path when it named a path alone.
enum class ConsoleReferenceKind : std::uint8_t { None, ScriptLocation, AssetPath };

/// Hard bound on captured entries; the ring drops the oldest entry once
/// full rather than growing. At
/// worst case (every field's static size) this is a few MB, acceptable for
/// an editor-only in-memory tool, never written to disk or streamed.
constexpr std::size_t kMaxConsoleEntries = 2048U;

/// Fixed capacity for message text; longer messages (chiefly Lua
/// tracebacks) are truncated with a trailing ellipsis marker. This is
/// presentation truncation of a diagnostic string, not truncation of
/// authored user data, so the atomic-write contract does not apply.
constexpr std::size_t kConsoleMessageCapacity = 512U;
constexpr std::size_t kConsoleChannelCapacity = 32U;
/// Fixed capacity for a script or asset path reference.
constexpr std::size_t kConsolePathCapacity = 192U;

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
  /// 64-bit FNV-1a of the level, channel and message, taken at capture:
  /// what the collapsed view groups identical entries by.
  std::uint64_t contentHash = 0U;

  ConsoleReferenceKind referenceKind = ConsoleReferenceKind::None;
  char referencePath[kConsolePathCapacity] = {};
  /// 1-based source line for ConsoleReferenceKind::ScriptLocation; -1 when
  /// no line number was found.
  int referenceLine = -1;

  /// Persistent id of the entity the diagnostic is about, or
  /// kInvalidPersistentId. Resolved against the attached World at click
  /// time (see console_capture_resolve_entity), so it stays valid across a
  /// scene reload that re-creates the entity under the same id.
  runtime::PersistentId entityPersistentId = runtime::kInvalidPersistentId;
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

/// Badge counters for surfacing severity while the panel is closed.
/// Counts entries at or
/// above the given level ingested since the last console_capture_mark_seen
/// call (each repeat of a collapsed entry still increments this once).
std::uint32_t console_capture_unseen_error_count() noexcept;
std::uint32_t console_capture_unseen_warning_count() noexcept;
/// Resets both unseen counters to zero (call when the panel becomes
/// visible/focused).
void console_capture_mark_seen() noexcept;

/// Writes the menu-bar status for `errors` and `warnings` unseen entries
/// ("1 error, 2 warnings", "3 warnings") into `out`; false with `out`
/// empty when both are zero, so the bar shows nothing while all is well.
/// A status that does not fit `capacity` is refused, `out` empty.
bool format_console_status(std::uint32_t errors, std::uint32_t warnings,
                           char *out, std::size_t capacity) noexcept;

/// Groups identical entries -- the same level, channel and message --
/// wherever they fall in the log, in first-seen order, as Unity's Console
/// Collapse does, rather than only a repeat that directly follows its
/// twin. Entries are matched by ConsoleEntry::contentHash, one table
/// lookup each: two different lines share a 64-bit FNV-1a fingerprint
/// with odds far below anything a display grouping needs to guard. Fixed
/// storage for kMaxConsoleEntries groups; allocates nothing.
class ConsoleCollapser final {
public:
  /// Forgets every group.
  void clear() noexcept;
  /// Counts `entry`, found at capture index `captureIndex`, into its
  /// group, opening one when it is the first of its kind. Past
  /// kMaxConsoleEntries groups a new kind is not grouped (never happens
  /// for a view over the capture ring, which holds no more entries).
  void add(const ConsoleEntry &entry, std::size_t captureIndex) noexcept;
  /// Groups so far.
  std::size_t size() const noexcept { return m_count; }
  /// Capture index of the group's first entry.
  std::size_t first_index(std::size_t group) const noexcept;
  /// Times the group's line was logged, every repeat included.
  std::uint32_t total(std::size_t group) const noexcept;

private:
  /// Open-addressed slots holding a group index + 1 (0 = empty); twice
  /// the group capacity keeps probes short.
  static constexpr std::size_t kSlots = 2U * kMaxConsoleEntries;
  static_assert((kSlots & (kSlots - 1U)) == 0U,
                "slots are indexed by masking, so they are a power of two");
  std::array<std::uint32_t, kSlots> m_slots{};
  std::array<std::uint64_t, kMaxConsoleEntries> m_hashes{};
  std::array<std::size_t, kMaxConsoleEntries> m_firstIndex{};
  std::array<std::uint32_t, kMaxConsoleEntries> m_totals{};
  std::size_t m_count = 0U;
};

/// Filter/search state the Console panel edits and applies at draw time;
/// kept separate from ConsoleEntry so filtering never mutates captured
/// data. Every field defaults to showing everything except Trace: Trace
/// carries the engine's periodic diagnostics (frame, slice and job
/// statistics), which belong to the stats overlay and profiler, so the
/// Console opens on user messages, warnings and errors as Unity's, Unreal's
/// and Godot's do. The Trace checkbox shows them; capture keeps them
/// either way.
struct ConsoleFilter final {
  bool showTrace = false;
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

/// Resolves an entry's entity against the given world by persistent id:
/// an entity is safe to select only when the world pointer is non-null and
/// an alive entity carries that id (a destroyed entity, or an index reused
/// by another, must not be selected in its place). Returns
/// runtime::kInvalidEntity when unsafe or unresolved.
runtime::Entity
console_capture_resolve_entity(runtime::PersistentId entityPersistentId,
                               const runtime::World *world) noexcept;

} // namespace engine::editor
