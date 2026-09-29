// Implements the editor's recently-used lists: read once from the save
// directory, most recent first, pruned of entries that no longer exist,
// and written back atomically after every change unless the stored file
// could not be read.

#include "editor_recent_list.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

namespace engine::editor {

namespace {

constexpr const char *kLogChannel = "editor";

/// Test-only override directory; empty means the per-user save directory.
char g_directoryOverride[900] = {};

std::size_t capacity_of(const RecentList &list) noexcept {
  return (list.capacity < kMaxRecentEntries) ? list.capacity
                                             : kMaxRecentEntries;
}

/// The directory the lists live in: the test override when set, otherwise
/// the per-user platform save directory.
bool resolve_directory(char *out, std::size_t capacity) noexcept {
  if (g_directoryOverride[0] != '\0') {
    const int written = std::snprintf(out, capacity, "%s", g_directoryOverride);
    return (written > 0) && (static_cast<std::size_t>(written) < capacity);
  }
  return core::platform_get_save_dir(out, capacity);
}

bool build_path(const RecentList &list, char *out,
                std::size_t capacity) noexcept {
  char directory[900] = {};
  if (!resolve_directory(directory, sizeof(directory))) {
    return false;
  }
  const int written =
      std::snprintf(out, capacity, "%s/%s", directory, list.fileName);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// Latches the list against writing back over a stored file this session
/// could not use, saying why once.
void refuse_stored_file(RecentList *list, const char *path,
                        const char *why) noexcept {
  list->loadFailed = true;
  char message[1200] = {};
  std::snprintf(message, sizeof(message),
                "recent list %s could not be used (%s); it starts empty and "
                "is not saved this session, so the stored file is kept",
                path, why);
  core::log_message(core::LogLevel::Warning, kLogChannel, message);
}

void persist(const RecentList &list) noexcept {
  // The stored list was never read this session, so the one in memory is
  // not a superset of it; writing would replace entries that may still be
  // good.
  if (list.loadFailed) {
    return;
  }
  char directory[900] = {};
  if (!resolve_directory(directory, sizeof(directory)) ||
      !core::create_directories_durably(directory)) {
    return;
  }
  char path[1024] = {};
  if (!build_path(list, path, sizeof(path))) {
    return;
  }

  core::JsonWriter writer{};
  writer.begin_object();
  writer.begin_array(list.arrayKey);
  for (std::size_t i = 0U; i < list.count; ++i) {
    writer.write_string_value(list.entries[i]);
  }
  writer.end_array();
  writer.end_object();
  if (writer.failed()) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "failed to serialize a recent list");
    return;
  }
  if (!core::atomic_write_file(path, writer.result(), writer.result_size())) {
    char message[1100] = {};
    std::snprintf(message, sizeof(message), "failed to write recent list %s",
                  path);
    core::log_message(core::LogLevel::Error, kLogChannel, message);
  }
}

} // namespace

bool recent_entry_is_file(const char *path) noexcept {
  std::error_code ec{};
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool recent_entry_exists(const char *path) noexcept {
  std::error_code ec{};
  return std::filesystem::exists(path, ec) && !ec;
}

void recent_list_load_once(RecentList *list) noexcept {
  if ((list == nullptr) || list->loaded) {
    return;
  }
  list->loaded = true;
  list->count = 0U;
  list->loadFailed = false;

  char path[1024] = {};
  if (!build_path(*list, path, sizeof(path))) {
    return;
  }
  static char buffer[kMaxStoredRecentListBytes + 1U] = {};
  std::size_t size = 0U;
  // Absent is the fresh-profile case: an empty list that saves normally.
  const core::FileReadResult read =
      core::read_whole_file(path, buffer, sizeof(buffer), &size);
  if (read == core::FileReadResult::Absent) {
    return;
  }
  if (read != core::FileReadResult::Ok) {
    refuse_stored_file(list, path,
                       (read == core::FileReadResult::TooLarge)
                           ? "larger than any list this editor writes"
                           : "read fault");
    return;
  }

  core::JsonParser parser{};
  core::JsonValue entries{};
  const core::JsonValue *root =
      parser.parse(buffer, size) ? parser.root() : nullptr;
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object) ||
      !parser.get_object_field(*root, list->arrayKey, &entries) ||
      (entries.type != core::JsonValue::Type::Array)) {
    refuse_stored_file(list, path, "not a recent list");
    return;
  }

  const std::size_t stored = parser.array_size(entries);
  const std::size_t capacity = capacity_of(*list);
  bool pruned = false;
  for (std::size_t i = 0U; (i < stored) && (list->count < capacity); ++i) {
    core::JsonValue element{};
    char entry[kMaxRecentPathLength] = {};
    if (!parser.get_array_element(entries, i, &element) ||
        !parser.copy_string(element, entry, sizeof(entry)) ||
        (entry[0] == '\0')) {
      pruned = true;
      continue;
    }
    // A moved or deleted entry is dropped rather than offered.
    if ((list->stillExists != nullptr) && !list->stillExists(entry)) {
      pruned = true;
      continue;
    }
    std::memcpy(list->entries[list->count], entry, sizeof(entry));
    ++list->count;
  }
  if (pruned) {
    persist(*list);
  }
}

void recent_list_add(RecentList *list, const char *path) noexcept {
  if ((list == nullptr) || (path == nullptr) || (path[0] == '\0')) {
    return;
  }
  const std::size_t length = std::strlen(path);
  if ((length >= kMaxRecentPathLength) || (capacity_of(*list) == 0U)) {
    return;
  }
  recent_list_load_once(list);
  // The entries after the new front, oldest dropped past capacity: shift
  // down over the old copy of `path` (or the last kept entry).
  std::size_t end = list->count;
  for (std::size_t i = 0U; i < list->count; ++i) {
    if (std::strcmp(list->entries[i], path) == 0) {
      end = i;
      break;
    }
  }
  if ((end == list->count) && (list->count == capacity_of(*list))) {
    end = list->count - 1U;
  } else if (end == list->count) {
    ++list->count;
  }
  for (std::size_t i = end; i > 0U; --i) {
    std::memcpy(list->entries[i], list->entries[i - 1U], kMaxRecentPathLength);
  }
  std::memcpy(list->entries[0], path, length + 1U);
  persist(*list);
}

void recent_list_remove(RecentList *list, const char *path) noexcept {
  if ((list == nullptr) || (path == nullptr)) {
    return;
  }
  recent_list_load_once(list);
  std::size_t kept = 0U;
  for (std::size_t i = 0U; i < list->count; ++i) {
    if (std::strcmp(list->entries[i], path) == 0) {
      continue;
    }
    if (kept != i) {
      std::memcpy(list->entries[kept], list->entries[i], kMaxRecentPathLength);
    }
    ++kept;
  }
  if (kept != list->count) {
    list->count = kept;
    persist(*list);
  }
}

std::size_t recent_list_count(RecentList *list) noexcept {
  recent_list_load_once(list);
  return (list != nullptr) ? list->count : 0U;
}

const char *recent_list_at(RecentList *list, std::size_t index) noexcept {
  recent_list_load_once(list);
  if ((list == nullptr) || (index >= list->count)) {
    return "";
  }
  return list->entries[index];
}

void recent_list_forget(RecentList *list) noexcept {
  if (list == nullptr) {
    return;
  }
  list->loaded = false;
  list->count = 0U;
  list->loadFailed = false;
}

void recent_lists_set_directory_override_for_tests(
    const char *directory) noexcept {
  std::snprintf(g_directoryOverride, sizeof(g_directoryOverride), "%s",
                (directory != nullptr) ? directory : "");
}

} // namespace engine::editor
