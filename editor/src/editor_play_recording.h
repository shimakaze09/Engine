// Declares play recording in the editor: Record Play starts Play with the
// session recorded (engine/runtime/play_recording.h) into the project's
// Recordings folder, and Replay starts Play replaying one, reporting in the
// Log whether it reproduced the recording and, if not, the first tick and
// state that differ. Reached from the Edit menu, the toolbar's REC and
// REPLAY cue, and the `demorec`, `demoplay` and `demostop` console
// commands, as Unreal's demo commands are named. Stop ends a recording or
// replay with its session.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::editor {

/// The project data subdirectory recordings live in.
inline constexpr const char *kRecordingsDirectory = "Recordings";

/// The longest recording name a command takes, without ".demo".
inline constexpr std::size_t kMaxRecordingName = 64U;

/// True when `name` can name a recording: 1 to kMaxRecordingName letters,
/// digits, '_', '-' or '.', not starting with '.'. A name that does not
/// fit is refused, never shortened, since it names the file.
bool recording_name_is_valid(const char *name) noexcept;

/// Writes `<directory>/<name>.demo` into `out`. False, with `out` emptied,
/// when the name is not valid or the path does not fit `capacity` whole.
bool recording_path_for_name(const char *directory, const char *name, char *out,
                             std::size_t capacity) noexcept;

/// The recordings in a directory, newest first.
struct RecordingList final {
  static constexpr std::size_t kMaxListed = 16U;
  /// File names without ".demo".
  char names[kMaxListed][kMaxRecordingName + 1U] = {};
  std::size_t count = 0U;
  /// More recordings exist than are listed.
  bool truncated = false;
};

/// Lists the ".demo" files directly in `directory`, newest (last written)
/// first, ties by name. A name that could not be typed back
/// (recording_name_is_valid) is skipped. An absent directory lists none.
RecordingList list_recordings(const char *directory) noexcept;

/// True when Record Play or Replay may start: a world is open and
/// stopped, with no Stop still being finished, and no recording or replay
/// is open.
bool recorded_play_can_start() noexcept;

/// Starts Play recorded to `<project>/Recordings/<name>.demo`, or to a
/// timestamped "Recording-YYYYMMDD-HHMMSS" name when `name` is null.
/// False, logged, with nothing started, when it cannot start, the name is
/// not valid or already a recording, or the recording cannot begin.
bool start_recorded_play(const char *name) noexcept;

/// Starts Play replaying `<project>/Recordings/<name>.demo`, or the newest
/// recording when `name` is null. False, logged, with nothing started,
/// when it cannot start, there is no such recording, or the recording is
/// refused.
bool start_replay(const char *name) noexcept;

/// Draws the Edit menu's recording items: Record Play, Replay Latest
/// Recording, and a Replay submenu listing the recordings.
void draw_recording_menu_items() noexcept;

/// Draws the toolbar's cue while a recording ("REC") or replay ("REPLAY")
/// is open; draws nothing otherwise.
void draw_recording_status() noexcept;

/// Registers `demorec [name]`, `demoplay [name]` and `demostop`. False
/// when the console refuses one.
bool register_recording_commands() noexcept;

} // namespace engine::editor
