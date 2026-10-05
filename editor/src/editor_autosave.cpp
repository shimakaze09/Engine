// Implements the editor's autosave and restart recovery (see
// editor_autosave.h).

#include "editor_autosave.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <system_error>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/project_data.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "editor_project_files.h"
#include "editor_scene_document.h"
#include "editor_session.h"

namespace engine::editor {

namespace {

constexpr const char *kLogChannel = "editor.autosave";
constexpr const char *kAutosaveFolder = "Autosave";
constexpr const char *kMarkerName = "session.lock";
constexpr const char *kManifestName = "autosave.json";
/// The manifest's format; a reader meeting another refuses it.
constexpr std::uint32_t kManifestVersion = 1U;
/// Larger than any manifest this editor writes.
constexpr std::size_t kMaxManifestBytes = 8192U;
constexpr std::uint64_t kNanosecondsPerMinute = 60ULL * 1000000000ULL;

struct AutosaveState {
  /// autosave_begin_session ran this session, whether or not it succeeded,
  /// so a missing data directory is reported once, not every frame.
  bool attempted = false;
  bool begun = false;
  bool fatalExit = false;
  /// <project data> and <project data>/Autosave, kept from the start of
  /// the session so its end does not depend on the project still being
  /// named.
  char dataDirectory[1024] = {};
  char directory[1100] = {};
  bool pending = false;
  AutosaveRecord pendingRecord{};
  char error[640] = {};
  /// The slot the next autosave writes: 'a' or 'b'.
  char nextSlot = 'a';
  /// When the next copy is due; 0 while nothing is unsaved.
  std::uint64_t dueAtNs = 0U;
  /// The manifest names a copy of this session's unsaved scene. Once the
  /// scene is saved it is forgotten, so a later crash never offers a copy
  /// older than the saved file.
  bool manifestLive = false;
};

AutosaveState g_state{};
int g_minutes = kDefaultAutosaveMinutes;

bool join(char *out, std::size_t capacity, const char *directory,
          const char *name) noexcept {
  const int written = std::snprintf(out, capacity, "%s/%s", directory, name);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

bool file_exists(const char *path) noexcept {
  std::error_code ec{};
  return std::filesystem::is_regular_file(std::filesystem::path(path), ec) &&
         !ec;
}

void remove_file(const char *path) noexcept {
  std::error_code ec{};
  static_cast<void>(std::filesystem::remove(std::filesystem::path(path), ec));
  if (ec) {
    char message[1300] = {};
    std::snprintf(message, sizeof(message), "could not remove %s", path);
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
  }
}

/// A copy the manifest may name: a relative path into the Autosave or
/// Recovery folder, with no parent step, so a damaged or hand-edited
/// manifest can never send a load outside the project data directory.
bool is_safe_copy_name(const char *file) noexcept {
  const bool inFolder = (std::strncmp(file, "Autosave/", 9U) == 0) ||
                        (std::strncmp(file, "Recovery/", 9U) == 0);
  return inFolder && (std::strstr(file, "..") == nullptr) &&
         (std::strchr(file, '\\') == nullptr) &&
         (std::strchr(file, ':') == nullptr);
}

void format_local_time(char *out, std::size_t capacity) noexcept {
  const std::tm local = local_time_now();
  if (std::strftime(out, capacity, "%Y-%m-%d %H:%M:%S", &local) == 0U) {
    out[0] = '\0';
  }
}

/// Fills the record's description of the open scene.
void describe_open_scene(AutosaveRecord *record, bool beforePlay) noexcept {
  std::snprintf(record->scenePath, sizeof(record->scenePath), "%s",
                scene_document_path());
  std::snprintf(record->sceneName, sizeof(record->sceneName), "%s",
                scene_document_display_name());
  format_local_time(record->savedAt, sizeof(record->savedAt));
  record->beforePlay = beforePlay;
}

/// Commits `record` as the manifest in `directory`.
bool write_manifest(const char *directory,
                    const AutosaveRecord &record) noexcept {
  char path[1200] = {};
  if (!join(path, sizeof(path), directory, kManifestName)) {
    return false;
  }
  core::JsonWriter writer{core::JsonLayout::Lines};
  writer.begin_object();
  writer.write_uint("autosave", kManifestVersion);
  writer.write_string("file", record.file);
  writer.write_string("scene", record.scenePath);
  writer.write_string("name", record.sceneName);
  writer.write_string("savedAt", record.savedAt);
  writer.write_bool("beforePlay", record.beforePlay);
  writer.end_object();
  if (writer.failed() ||
      !core::atomic_write_file(path, writer.result(), writer.result_size())) {
    char message[1300] = {};
    std::snprintf(message, sizeof(message),
                  "could not write the autosave manifest %s", path);
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
    return false;
  }
  return true;
}

/// Reads one string member whole; an absent or overlong one fails.
bool read_string(const core::JsonParser &parser, const core::JsonValue &root,
                 const char *key, char *out, std::size_t capacity) noexcept {
  core::JsonValue value{};
  return parser.get_object_field(root, key, &value) &&
         parser.copy_string_strict(value, out, capacity);
}

/// Reads the manifest at `path` into `out`. False, with a Warning for a
/// present file it cannot use, when there is no manifest this editor
/// reads.
bool read_manifest(const char *path, AutosaveRecord *out) noexcept {
  static char buffer[kMaxManifestBytes + 1U] = {};
  std::size_t size = 0U;
  const core::FileReadResult read =
      core::read_whole_file(path, buffer, sizeof(buffer), &size);
  if (read == core::FileReadResult::Absent) {
    return false;
  }
  core::JsonParser parser{};
  core::JsonReadTracker tracker{};
  const core::JsonValue *root =
      ((read == core::FileReadResult::Ok) && parser.parse(buffer, size))
          ? parser.root()
          : nullptr;
  if ((root != nullptr) && tracker.reset_for(buffer, size)) {
    parser.set_read_tracker(&tracker);
  }
  AutosaveRecord record{};
  core::JsonValue version{};
  core::JsonValue beforePlay{};
  std::uint32_t versionNumber = 0U;
  const bool ok =
      (root != nullptr) && (root->type == core::JsonValue::Type::Object) &&
      parser.get_object_field(*root, "autosave", &version) &&
      parser.as_uint(version, &versionNumber) &&
      (versionNumber == kManifestVersion) &&
      read_string(parser, *root, "file", record.file, sizeof(record.file)) &&
      is_safe_copy_name(record.file) &&
      read_string(parser, *root, "scene", record.scenePath,
                  sizeof(record.scenePath)) &&
      read_string(parser, *root, "name", record.sceneName,
                  sizeof(record.sceneName)) &&
      read_string(parser, *root, "savedAt", record.savedAt,
                  sizeof(record.savedAt)) &&
      parser.get_object_field(*root, "beforePlay", &beforePlay) &&
      parser.as_bool(beforePlay, &record.beforePlay);
  if (!ok) {
    char message[1300] = {};
    std::snprintf(message, sizeof(message),
                  "the autosave manifest %s is not one this editor reads; "
                  "nothing is offered for recovery from it",
                  path);
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
    return false;
  }
  static_cast<void>(core::json_log_unread_members(
      *root, tracker, kLogChannel, path, "the next autosave drops it"));
  *out = record;
  return true;
}

/// Writes the session marker into `directory`.
bool write_marker(const char *directory) noexcept {
  char path[1200] = {};
  static constexpr char kText[] = "An editor session is running on this "
                                  "project, or the last one did not end "
                                  "cleanly.\n";
  return join(path, sizeof(path), directory, kMarkerName) &&
         core::atomic_write_file(path, kText, sizeof(kText) - 1U);
}

/// Whether the unsaved scene can be read now: the Play snapshot during
/// Play, otherwise the World when it can be saved.
bool can_capture(bool *outFromSnapshot) noexcept {
  const EditorSession &session = editor_session();
  if (session.world == nullptr) {
    return false;
  }
  if (session.playState != PlayState::Stopped) {
    *outFromSnapshot = true;
    return session.hasPlaySnapshot &&
           (session.playSnapshotWorld == session.world) &&
           (session.playSnapshotBuffer != nullptr);
  }
  *outFromSnapshot = false;
  return world_can_load_scene();
}

/// Removes this session's manifest.
void forget_manifest() noexcept {
  char manifest[1200] = {};
  if (join(manifest, sizeof(manifest), g_state.directory, kManifestName)) {
    remove_file(manifest);
  }
  g_state.manifestLive = false;
}

} // namespace

bool autosave_begin_session() noexcept {
  if (g_state.attempted) {
    return g_state.begun;
  }
  g_state.attempted = true;
  if (!core::project_data_dir(g_state.dataDirectory,
                              sizeof(g_state.dataDirectory)) ||
      !project_data_subdirectory(kAutosaveFolder, g_state.directory,
                                 sizeof(g_state.directory))) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "autosave is off this session: the project has no "
                      "data directory");
    return false;
  }
  char marker[1200] = {};
  char manifest[1200] = {};
  if (!join(marker, sizeof(marker), g_state.directory, kMarkerName) ||
      !join(manifest, sizeof(manifest), g_state.directory, kManifestName)) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "autosave is off this session: its folder's path is "
                      "too long");
    return false;
  }

  const bool endedUncleanly = file_exists(marker);
  AutosaveRecord record{};
  const bool haveRecord = read_manifest(manifest, &record);
  if (haveRecord && (std::strcmp(record.file, "Autosave/scene-a.scene") == 0)) {
    g_state.nextSlot = 'b';
  }
  char copy[1400] = {};
  const bool copyThere =
      haveRecord &&
      join(copy, sizeof(copy), g_state.dataDirectory, record.file) &&
      file_exists(copy);
  if (endedUncleanly && copyThere) {
    g_state.pending = true;
    g_state.manifestLive = true;
    g_state.pendingRecord = record;
    char message[1700] = {};
    std::snprintf(message, sizeof(message),
                  "the last editor session did not end cleanly; its copy of "
                  "%s from %s is offered for recovery (%s)",
                  record.sceneName, record.savedAt, copy);
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
  } else if (endedUncleanly) {
    core::log_message(core::LogLevel::Info, kLogChannel,
                      "the last editor session did not end cleanly; it left "
                      "no unsaved scene to recover");
  } else if (haveRecord) {
    // A clean end removes the manifest; one left behind names nothing
    // the author is owed.
    remove_file(manifest);
  }

  if (!write_marker(g_state.directory)) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "could not write the session marker; a crash this "
                      "session will not offer recovery at the next launch");
  }
  g_state.begun = true;
  return true;
}

void autosave_end_session() noexcept {
  if (g_state.begun && !g_state.fatalExit && !g_state.pending) {
    char marker[1200] = {};
    // The manifest goes first: interrupted between the two, the next
    // launch finds a marker with nothing to offer, never a copy the
    // author already chose to leave.
    forget_manifest();
    if (join(marker, sizeof(marker), g_state.directory, kMarkerName)) {
      remove_file(marker);
    }
  }
  autosave_reset();
}

void autosave_note_fatal_exit() noexcept { g_state.fatalExit = true; }

bool autosave_tick(std::uint64_t nowNs) noexcept {
  if (!g_state.begun || g_state.pending) {
    return false;
  }
  if (!scene_document_is_dirty()) {
    g_state.dueAtNs = 0U;
    if (g_state.manifestLive) {
      forget_manifest();
    }
    return false;
  }
  if (g_minutes <= 0) {
    g_state.dueAtNs = 0U;
    return false;
  }
  const std::uint64_t interval =
      static_cast<std::uint64_t>(g_minutes) * kNanosecondsPerMinute;
  if (g_state.dueAtNs == 0U) {
    g_state.dueAtNs = nowNs + interval;
    return false;
  }
  bool fromSnapshot = false;
  if ((nowNs < g_state.dueAtNs) || !can_capture(&fromSnapshot)) {
    return false;
  }
  // A failed copy waits a whole interval too, so a full disk is reported
  // every few minutes rather than every frame.
  g_state.dueAtNs = nowNs + interval;
  return autosave_write_now();
}

bool autosave_write_now() noexcept {
  bool fromSnapshot = false;
  if (!g_state.begun || g_state.pending || !scene_document_is_dirty() ||
      !can_capture(&fromSnapshot)) {
    return false;
  }
  const EditorSession &session = editor_session();
  std::unique_ptr<char[]> owned{};
  const char *bytes = nullptr;
  std::size_t size = 0U;
  if (fromSnapshot) {
    bytes = session.playSnapshotBuffer.get();
    size = session.playSnapshotSize;
  } else if (runtime::save_scene(*session.world, &owned, &size)) {
    bytes = owned.get();
  }

  AutosaveRecord record{};
  std::snprintf(record.file, sizeof(record.file), "%s/scene-%c.scene",
                kAutosaveFolder, g_state.nextSlot);
  char slot[1400] = {};
  if ((bytes == nullptr) ||
      !join(slot, sizeof(slot), g_state.dataDirectory, record.file) ||
      !core::atomic_write_file(slot, bytes, size)) {
    char message[1600] = {};
    std::snprintf(message, sizeof(message),
                  "autosave of %s failed; the last copy is kept",
                  scene_document_display_name());
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
    return false;
  }
  describe_open_scene(&record, fromSnapshot);
  if (!write_manifest(g_state.directory, record)) {
    return false;
  }
  g_state.nextSlot = (g_state.nextSlot == 'a') ? 'b' : 'a';
  g_state.manifestLive = true;
  char message[1600] = {};
  std::snprintf(message, sizeof(message), "autosaved %s to %s",
                record.sceneName, slot);
  core::log_message(core::LogLevel::Trace, kLogChannel, message);
  return true;
}

bool autosave_record_copy(const char *path, bool beforePlay) noexcept {
  char dataDirectory[1024] = {};
  char directory[1100] = {};
  if ((path == nullptr) ||
      !core::project_data_dir(dataDirectory, sizeof(dataDirectory)) ||
      !project_data_subdirectory(kAutosaveFolder, directory,
                                 sizeof(directory))) {
    return false;
  }
  const std::size_t prefix = std::strlen(dataDirectory);
  AutosaveRecord record{};
  const bool under = (std::strncmp(path, dataDirectory, prefix) == 0) &&
                     ((path[prefix] == '/') || (path[prefix] == '\\'));
  const int written = under ? std::snprintf(record.file, sizeof(record.file),
                                            "%s", path + prefix + 1U)
                            : -1;
  for (char *c = record.file; *c != '\0'; ++c) {
    *c = (*c == '\\') ? '/' : *c;
  }
  if ((written <= 0) ||
      (static_cast<std::size_t>(written) >= sizeof(record.file)) ||
      !is_safe_copy_name(record.file)) {
    char message[1300] = {};
    std::snprintf(message, sizeof(message),
                  "%s is not a copy the next launch can offer", path);
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
    return false;
  }
  describe_open_scene(&record, beforePlay);
  // A copy recorded outside a begun session still has to be found at the
  // next launch, which looks for the marker first.
  return write_manifest(directory, record) && write_marker(directory);
}

bool autosave_recovery_pending() noexcept { return g_state.pending; }

const AutosaveRecord *autosave_pending_recovery() noexcept {
  return g_state.pending ? &g_state.pendingRecord : nullptr;
}

bool autosave_choose_recovery(RecoveryChoice choice) noexcept {
  if (!g_state.pending) {
    return false;
  }
  const AutosaveRecord &record = g_state.pendingRecord;
  if (choice == RecoveryChoice::Discard) {
    forget_manifest();
    g_state.pending = false;
    g_state.error[0] = '\0';
    core::log_message(core::LogLevel::Info, kLogChannel,
                      "the unsaved copy from the last session was discarded");
    return true;
  }

  char copy[1400] = {};
  const char *authored = "";
  if (choice == RecoveryChoice::Recover) {
    authored = record.scenePath;
    // The project may have moved since: a scene outside its assets comes
    // back untitled, for Save As to place.
    if ((authored[0] != '\0') && !scene_path_passes_jail(authored)) {
      char message[1100] = {};
      std::snprintf(message, sizeof(message),
                    "%s is outside the project's assets now; the copy is "
                    "recovered as an untitled scene",
                    authored);
      core::log_message(core::LogLevel::Warning, kLogChannel, message);
      authored = "";
    }
  }
  if (!join(copy, sizeof(copy), g_state.dataDirectory, record.file) ||
      !perform_scene_recover(copy, authored)) {
    std::snprintf(g_state.error, sizeof(g_state.error),
                  "%s could not be loaded; Discard to continue without it",
                  record.file);
    core::log_message(core::LogLevel::Error, kLogChannel, g_state.error);
    return false;
  }
  g_state.pending = false;
  g_state.error[0] = '\0';
  g_state.dueAtNs = 0U;
  // Still the only copy of what was recovered until it is saved.
  g_state.manifestLive = true;
  char message[1600] = {};
  std::snprintf(message, sizeof(message), "%s the copy of %s from %s",
                (choice == RecoveryChoice::Recover) ? "recovered"
                                                    : "opened for inspection",
                record.sceneName, record.savedAt);
  core::log_message(core::LogLevel::Info, kLogChannel, message);
  return true;
}

const char *autosave_recovery_error() noexcept { return g_state.error; }

int autosave_minutes() noexcept { return g_minutes; }

void set_autosave_minutes(int minutes) noexcept {
  g_minutes = (minutes < 0)                     ? 0
              : (minutes > kMaxAutosaveMinutes) ? kMaxAutosaveMinutes
                                                : minutes;
  g_state.dueAtNs = 0U;
}

void autosave_reset() noexcept { g_state = AutosaveState{}; }

} // namespace engine::editor
