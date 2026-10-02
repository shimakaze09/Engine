// Implements play recording in the editor: the Recordings folder and its
// names, starting a recorded or replayed Play, the Edit menu items, the
// toolbar cue and the demo console commands.

#include "editor_play_recording.h"

#include "editor_project_files.h"
#include "editor_session.h"
#include "editor_shortcuts.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include "engine/core/console.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/runtime/play_recording.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace engine::editor {

namespace {

constexpr std::size_t kMaxPath = 1024U;
constexpr std::size_t kExtensionLength = 5U; // ".demo"

void log_refusal(const char *what, const char *reason) noexcept {
  char message[kMaxPath + 128U] = {};
  std::snprintf(message, sizeof(message), "%s refused: %s", what, reason);
  core::log_message(core::LogLevel::Warning, "editor", message);
}

bool recordings_directory(char *out, std::size_t capacity) noexcept {
  return project_data_subdirectory(kRecordingsDirectory, out, capacity);
}

/// Plays after a recording or replay began, closing it again when Play
/// did not start, so it cannot attach to a later session.
bool start_play_for_log(const char *what) noexcept {
  start_play_mode();
  if (editor_session().playState == PlayState::Playing) {
    return true;
  }
  static_cast<void>(runtime::end_play_log());
  log_refusal(what, "Play did not start; the Log says why");
  return false;
}

void demorec_command(const char *const *args, int argCount, void *) noexcept {
  if (argCount > 2) {
    core::console_print("usage: demorec [name]");
    return;
  }
  static_cast<void>(start_recorded_play((argCount == 2) ? args[1] : nullptr));
}

void demoplay_command(const char *const *args, int argCount, void *) noexcept {
  if (argCount > 2) {
    core::console_print("usage: demoplay [name]");
    return;
  }
  static_cast<void>(start_replay((argCount == 2) ? args[1] : nullptr));
}

void demostop_command(const char *const *, int, void *) noexcept {
  if (editor_session().playState == PlayState::Stopped) {
    core::console_print("demostop: nothing is playing");
    return;
  }
  stop_play_mode();
}

} // namespace

bool recording_name_is_valid(const char *name) noexcept {
  // A leading '.' would hide the file or name a directory entry.
  return (name != nullptr) && (name[0] != '.') &&
         core::name_token_is_valid(name, kMaxRecordingName);
}

bool recording_path_for_name(const char *directory, const char *name, char *out,
                             std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((directory == nullptr) || !recording_name_is_valid(name)) {
    return false;
  }
  const int written = std::snprintf(out, capacity, "%s/%s%s", directory, name,
                                    runtime::kPlayRecordingExtension);
  if ((written < 0) || (static_cast<std::size_t>(written) >= capacity)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

RecordingList list_recordings(const char *directory) noexcept {
  RecordingList list{};
  if (directory == nullptr) {
    return list;
  }
  // The listed names' write times, parallel to list.names.
  std::filesystem::file_time_type written[RecordingList::kMaxListed]{};
  std::error_code ec{};
  std::filesystem::directory_iterator it(std::filesystem::path(directory), ec);
  for (; !ec && (it != std::filesystem::directory_iterator());
       it.increment(ec)) {
    std::error_code entryEc{};
    if (!it->is_regular_file(entryEc) ||
        (it->path().extension() != runtime::kPlayRecordingExtension)) {
      continue;
    }
    const std::string file = it->path().filename().string();
    const std::string name = file.substr(0U, file.size() - kExtensionLength);
    if (!recording_name_is_valid(name.c_str())) {
      continue;
    }
    const std::filesystem::file_time_type time = it->last_write_time(entryEc);
    // Insertion into the newest-first list; ties go by name.
    std::size_t at = 0U;
    while ((at < list.count) &&
           ((written[at] > time) ||
            ((written[at] == time) &&
             (std::strcmp(list.names[at], name.c_str()) < 0)))) {
      ++at;
    }
    if (at == RecordingList::kMaxListed) {
      list.truncated = true;
      continue;
    }
    if (list.count == RecordingList::kMaxListed) {
      list.truncated = true;
      --list.count;
    }
    for (std::size_t i = list.count; i > at; --i) {
      written[i] = written[i - 1U];
      std::memcpy(list.names[i], list.names[i - 1U], sizeof(list.names[i]));
    }
    written[at] = time;
    std::snprintf(list.names[at], sizeof(list.names[at]), "%s", name.c_str());
    ++list.count;
  }
  return list;
}

bool recorded_play_can_start() noexcept {
  const EditorSession &session = editor_session();
  return (session.world != nullptr) && !session.worldRestoreFailed &&
         (session.playState == PlayState::Stopped) &&
         !session.playStopPending && !runtime::play_recording_active() &&
         !runtime::play_replay_active();
}

bool start_recorded_play(const char *name) noexcept {
  constexpr const char *kWhat = "Record Play";
  if (!recorded_play_can_start()) {
    log_refusal(kWhat, "it starts only while stopped, with a scene open and "
                       "no recording or replay running");
    return false;
  }
  if ((name != nullptr) && !recording_name_is_valid(name)) {
    log_refusal(kWhat,
                "a recording name is 1 to 64 bytes of letters (CJK, kana "
                "and Hangul included), digits, '_', '-' or '.', not "
                "starting with '.'");
    return false;
  }
  char directory[kMaxPath] = {};
  if (!recordings_directory(directory, sizeof(directory))) {
    log_refusal(kWhat, "the project's Recordings folder is unavailable");
    return false;
  }
  char path[kMaxPath] = {};
  const bool named =
      (name != nullptr)
          ? recording_path_for_name(directory, name, path, sizeof(path))
          : next_timestamped_path(directory, "Recording",
                                  runtime::kPlayRecordingExtension,
                                  local_time_now(), &project_file_exists,
                                  nullptr, path, sizeof(path));
  if (!named) {
    log_refusal(kWhat, "no recording path fits under the project's "
                       "Recordings folder");
    return false;
  }
  if (!runtime::begin_play_recording(path)) {
    return false; // begin_play_recording logged why
  }
  return start_play_for_log(kWhat);
}

bool start_replay(const char *name) noexcept {
  constexpr const char *kWhat = "Replay";
  if (!recorded_play_can_start()) {
    log_refusal(kWhat, "it starts only while stopped, with a scene open and "
                       "no recording or replay running");
    return false;
  }
  if ((name != nullptr) && !recording_name_is_valid(name)) {
    log_refusal(kWhat, "that is not a recording name");
    return false;
  }
  char directory[kMaxPath] = {};
  if (!recordings_directory(directory, sizeof(directory))) {
    log_refusal(kWhat, "the project's Recordings folder is unavailable");
    return false;
  }
  const RecordingList list = list_recordings(directory);
  const char *chosen = name;
  if (chosen == nullptr) {
    if (list.count == 0U) {
      log_refusal(kWhat, "the project has no recordings yet");
      return false;
    }
    chosen = list.names[0];
  }
  char path[kMaxPath] = {};
  if (!recording_path_for_name(directory, chosen, path, sizeof(path))) {
    log_refusal(kWhat, "the recording's path does not fit");
    return false;
  }
  if (!runtime::begin_play_replay(path)) {
    return false; // begin_play_replay logged why
  }
  return start_play_for_log(kWhat);
}

void draw_recording_menu_items() noexcept {
  editor_action_menu_item(EditorAction::RecordPlay);
  editor_action_menu_item(EditorAction::ReplayLatest);
  if (!ImGui::BeginMenu("Replay", recorded_play_can_start())) {
    return;
  }
  char directory[kMaxPath] = {};
  const RecordingList list = recordings_directory(directory, sizeof(directory))
                                 ? list_recordings(directory)
                                 : RecordingList{};
  if (list.count == 0U) {
    ImGui::TextDisabled("No recordings yet: Record Play makes one");
  }
  for (std::size_t i = 0U; i < list.count; ++i) {
    if (ImGui::MenuItem(list.names[i])) {
      static_cast<void>(start_replay(list.names[i]));
    }
  }
  if (list.truncated) {
    ImGui::TextDisabled("Older recordings: demoplay <name> in the Log");
  }
  ImGui::EndMenu();
}

void draw_recording_status() noexcept {
  const bool recording = runtime::play_recording_active();
  if (!recording && !runtime::play_replay_active()) {
    return;
  }
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (recording) {
    ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "REC");
    ImGui::SetItemTooltip("This session is being recorded; Stop saves it to "
                          "the project's Recordings folder");
  } else {
    ImGui::TextColored(ImVec4(0.35F, 0.8F, 0.95F, 1.0F), "REPLAY");
    ImGui::SetItemTooltip("This session replays a recording; the Log says "
                          "whether it matched");
  }
}

bool register_recording_commands() noexcept {
  const bool rec = core::console_register_command(
      "demorec", &demorec_command, nullptr,
      "Play and record the session into the project's Recordings folder "
      "(demorec [name])");
  const bool play = core::console_register_command(
      "demoplay", &demoplay_command, nullptr,
      "Play a recording back and check it matches (demoplay [name]; the "
      "newest without one)");
  const bool stop = core::console_register_command(
      "demostop", &demostop_command, nullptr,
      "Stop the session, saving a recording or ending a replay");
  return rec && play && stop;
}

} // namespace engine::editor
