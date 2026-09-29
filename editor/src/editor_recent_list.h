// Declares the editor's recently-used list: the paths a user opened last,
// most recent first, kept per user in the platform save directory. Recent
// scenes and recent projects are two lists of this one kind. Each is read
// once per session, drops entries that no longer exist, and is written
// back through a staged atomic replacement after every change; a stored
// list the session could not read is never overwritten, so a transient
// read fault cannot erase it.

#pragma once

#include <cstddef>

namespace engine::editor {

/// Most entries any list keeps.
inline constexpr std::size_t kMaxRecentEntries = 16U;
/// Longest path, terminator included, a list stores; a longer one is not
/// added, since a cut path names something else.
inline constexpr std::size_t kMaxRecentPathLength = 512U;

/// Longest stored list the reader accepts: every entry at its longest
/// path with each character escaped (the writer escapes path separators),
/// plus the JSON around them. A larger file is not one this editor wrote,
/// and is kept untouched.
inline constexpr std::size_t kMaxStoredRecentListBytes =
    (kMaxRecentEntries * ((2U * kMaxRecentPathLength) + 4U)) + 256U;

/// One list: what it is stored as, how long it may grow, which entries
/// are still worth offering, and its session cache.
struct RecentList final {
  /// The file in the save directory ("editor_recent_scenes.json").
  const char *fileName = "";
  /// The JSON array holding the paths ("scenes").
  const char *arrayKey = "";
  /// Entries kept, at most kMaxRecentEntries.
  std::size_t capacity = kMaxRecentEntries;
  /// True for an entry still worth offering; the others are dropped on
  /// load. Null keeps every entry.
  bool (*stillExists)(const char *path) noexcept = nullptr;

  char entries[kMaxRecentEntries][kMaxRecentPathLength] = {};
  std::size_t count = 0U;
  bool loaded = false;
  /// The stored file exists but could not be read this session, so the
  /// list is not written back (it would replace entries not yet seen).
  bool loadFailed = false;
};

/// Ready-made RecentList::stillExists checks: a regular file (a scene), or
/// anything at all (a project, as its directory or its document).
bool recent_entry_is_file(const char *path) noexcept;
bool recent_entry_exists(const char *path) noexcept;

/// Reads the stored list once per session; later calls do nothing.
void recent_list_load_once(RecentList *list) noexcept;
/// Moves `path` to the front (adding it when absent, dropping the oldest
/// past capacity) and writes the list. An empty or overlong path is
/// ignored.
void recent_list_add(RecentList *list, const char *path) noexcept;
/// Drops `path` when present and writes the list.
void recent_list_remove(RecentList *list, const char *path) noexcept;
/// The entry count, loading the list first.
std::size_t recent_list_count(RecentList *list) noexcept;
/// The entry at `index`, most recent first; "" past the end.
const char *recent_list_at(RecentList *list, std::size_t index) noexcept;
/// Forgets the session cache, so the next access reads the stored file
/// again.
void recent_list_forget(RecentList *list) noexcept;

/// Test-only: stores every list under `directory` instead of the per-user
/// save directory; "" restores the default. Lists already read keep their
/// cache until recent_list_forget.
void recent_lists_set_directory_override_for_tests(
    const char *directory) noexcept;

} // namespace engine::editor
