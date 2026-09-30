// Implements the editor's recovery copy of an unsaved scene (see
// editor_recovery.h).

#include "editor_recovery.h"

#include <cstdio>

#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "editor_project_files.h"
#include "editor_scene_document.h"
#include "editor_session.h"

namespace engine::editor {

namespace {

constexpr const char *kRecoveryDirectory = "Recovery";

// The scene's name as a file stem: its display name up to the extension,
// with anything but letters, digits, '-' and '_' replaced, so a name that
// holds a separator can never escape the Recovery folder.
void recovery_stem(char *out, std::size_t capacity) noexcept {
  const char *name = scene_document_display_name();
  std::size_t length = 0U;
  for (const char *c = name; (*c != '\0') && (*c != '.') &&
                             (length + 1U < capacity) && (length < 64U);
       ++c) {
    const char ch = *c;
    const bool keep =
        ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z')) ||
        ((ch >= '0') && (ch <= '9')) || (ch == '-') || (ch == '_');
    out[length++] = keep ? ch : '_';
  }
  out[length] = '\0';
  if (length == 0U) {
    std::snprintf(out, capacity, "%s", "Untitled");
  }
}

// Brings a World a fatal left mid-frame back to its Input phase, where the
// scene serializer reads it. No job is still running: the pipeline waits
// for the frame's jobs before it reports a fatal. A World stopped inside
// end-play is not forced: leaving that phase flushes its pending destroys,
// which would change the scene being saved.
bool settle_world(runtime::World &world) noexcept {
  if (world.current_phase() == runtime::WorldPhase::EndPlay) {
    return false;
  }
  world.end_frame_phase();
  return world.current_phase() == runtime::WorldPhase::Input;
}

} // namespace

bool write_recovery_copy(char *outPath, std::size_t capacity) noexcept {
  if ((outPath == nullptr) || (capacity == 0U)) {
    return false;
  }
  outPath[0] = '\0';
  EditorSession &session = editor_session();
  if (!session.initialized || (session.world == nullptr) ||
      !scene_document_is_dirty()) {
    return false;
  }

  char directory[512] = {};
  char stem[72] = {};
  recovery_stem(stem, sizeof(stem));
  char path[512] = {};
  if (!project_data_subdirectory(kRecoveryDirectory, directory,
                                 sizeof(directory)) ||
      !next_timestamped_path(directory, stem, ".scene", local_time_now(),
                             &project_file_exists, nullptr, path,
                             sizeof(path))) {
    core::log_message(core::LogLevel::Error, "editor",
                      "the unsaved scene could not be saved for recovery: "
                      "no Recovery folder or free file name");
    return false;
  }

  // During Play the World holds the play session; what the author has not
  // saved is the scene as it was before Play, which the snapshot holds.
  const bool playing = session.playState != PlayState::Stopped;
  const bool fromSnapshot = playing && session.hasPlaySnapshot &&
                            (session.playSnapshotWorld == session.world) &&
                            (session.playSnapshotBuffer != nullptr);
  bool written = false;
  if (fromSnapshot) {
    written = core::atomic_write_file(path, session.playSnapshotBuffer.get(),
                                      session.playSnapshotSize);
  } else {
    written = settle_world(*session.world) &&
              runtime::save_scene(*session.world, path);
  }
  if (!written) {
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "the unsaved scene could not be saved for recovery to %s",
                  path);
    core::log_message(core::LogLevel::Error, "editor", message);
    return false;
  }

  const int length = std::snprintf(outPath, capacity, "%s", path);
  if ((length < 0) || (static_cast<std::size_t>(length) >= capacity)) {
    outPath[0] = '\0';
  }
  char message[640] = {};
  std::snprintf(message, sizeof(message),
                "the unsaved scene%s was saved for recovery to %s",
                fromSnapshot ? " (as it was before Play)" : "", path);
  core::log_message(core::LogLevel::Warning, "editor", message);
  return true;
}

} // namespace engine::editor
