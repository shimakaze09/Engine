// Verifies play recording in the editor, headless (no pipeline frame runs):
// recording names are refused whole when they could not name a file; the
// Recordings folder lists its .demo files newest first, capped with the
// rest reported; Record Play begins a recording under the project's
// Recordings folder and enters Play, and refuses while playing, before a
// Stop is finished, without a world, or over an existing recording; Replay
// with no name takes the newest recording and enters Play replaying it;
// the demo console commands and the Edit menu actions reach the same
// paths and are disabled whenever Record Play could not start.

#include "editor_play_recording.h"
#include "editor_project_files.h"
#include "editor_session.h"
#include "editor_shortcuts.h"

#include "engine/core/console.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/project_data.h"
#include "engine/runtime/play_recording.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

using namespace engine::editor;
namespace fs = std::filesystem;

constexpr const char *kScratch = "engine_editor_play_recording_test";

bool touch(const fs::path &path, int ageSeconds) noexcept {
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "x";
    if (!out) {
      return false;
    }
  }
  std::error_code ec{};
  fs::last_write_time(
      path, fs::file_time_type::clock::now() - std::chrono::seconds(ageSeconds),
      ec);
  return !ec;
}

std::string names_of(const RecordingList &list) noexcept {
  std::string joined{};
  for (std::size_t i = 0U; i < list.count; ++i) {
    joined += (joined.empty() ? "" : " ") + std::string(list.names[i]);
  }
  return joined;
}

void check_names(engine::tests::TestContext &t) noexcept {
  const std::string longest(kMaxRecordingName, 'a');
  const std::string tooLong(kMaxRecordingName + 1U, 'a');
  t.check(recording_name_is_valid("boss-fight_2.take1") &&
              recording_name_is_valid(longest.c_str()),
          "letters, digits, '_', '-' and '.' up to 64 characters name one");
  t.check(!recording_name_is_valid(nullptr) && !recording_name_is_valid("") &&
              !recording_name_is_valid(".hidden") &&
              !recording_name_is_valid("a/b") &&
              !recording_name_is_valid("a b") &&
              !recording_name_is_valid(tooLong.c_str()),
          "empty, dotted, separated, spaced or 65-character names are refused");

  char path[32] = {};
  t.check(recording_path_for_name("rec", "take1", path, sizeof(path)) &&
              (std::strcmp(path, "rec/take1.demo") == 0),
          "a name becomes <directory>/<name>.demo");
  char exact[15] = {};
  t.check(recording_path_for_name("rec", "take1", exact, sizeof(exact)),
          "a path that fits exactly is written");
  char shortBuffer[14] = {};
  t.check(!recording_path_for_name("rec", "take1", shortBuffer,
                                   sizeof(shortBuffer)) &&
              (shortBuffer[0] == '\0'),
          "one that does not fit is refused whole");
  t.check(!recording_path_for_name("rec", "../x", path, sizeof(path)) &&
              (path[0] == '\0'),
          "an invalid name writes no path");
}

void check_listing(engine::tests::TestContext &t) noexcept {
  const fs::path directory = fs::path(kScratch) / "list";
  std::error_code ec{};
  fs::remove_all(directory, ec);
  t.check(list_recordings(directory.string().c_str()).count == 0U,
          "an absent folder lists nothing");
  fs::create_directories(directory, ec);
  t.check(touch(directory / "old.demo", 300) &&
              touch(directory / "new.demo", 10) &&
              touch(directory / "mid.demo", 100) &&
              touch(directory / "mid.input", 1) &&
              touch(directory / "notes.txt", 1) &&
              touch(directory / "bad name.demo", 1) &&
              touch(directory / ".hidden.demo", 1),
          "stage the folder");
  const RecordingList three = list_recordings(directory.string().c_str());
  t.check((names_of(three) == "new mid old") && !three.truncated,
          "only .demo files a name can reach are listed, newest first");

  for (int i = 0; i < 16; ++i) {
    char name[32] = {};
    std::snprintf(name, sizeof(name), "take%02d.demo", i);
    t.check(touch(directory / name, 1000 + i), "stage an older recording");
  }
  const RecordingList capped = list_recordings(directory.string().c_str());
  t.check((capped.count == RecordingList::kMaxListed) && capped.truncated &&
              (std::strcmp(capped.names[0], "new") == 0) &&
              (std::strcmp(capped.names[15], "take12") == 0),
          "nineteen recordings list the newest sixteen and say more exist");
  fs::remove_all(directory, ec);
}

void check_record_and_replay(engine::tests::TestContext &t) noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    t.fail("allocate a world");
    return;
  }
  EditorSession &session = editor_session();
  session.world = nullptr;
  t.check(!recorded_play_can_start() &&
              !editor_action_enabled(EditorAction::RecordPlay) &&
              !start_recorded_play("take1"),
          "nothing records without a world");

  session.world = world.get();
  session.playState = PlayState::Stopped;
  t.check(recorded_play_can_start() &&
              editor_action_enabled(EditorAction::RecordPlay) &&
              editor_action_enabled(EditorAction::ReplayLatest),
          "stopped with a world, Record Play and Replay are offered");
  t.check(!start_recorded_play("bad/name") &&
              (session.playState == PlayState::Stopped) &&
              !engine::runtime::play_recording_active(),
          "a name that cannot be a file starts nothing");

  char directory[1024] = {};
  t.check(project_data_subdirectory(kRecordingsDirectory, directory,
                                    sizeof(directory)),
          "the project's Recordings folder is made");
  t.check(start_recorded_play("take1") &&
              (session.playState == PlayState::Playing) &&
              engine::runtime::play_recording_active(),
          "Record Play begins the recording and enters Play");
  t.check(!recorded_play_can_start() &&
              !editor_action_enabled(EditorAction::RecordPlay) &&
              !start_replay(nullptr),
          "nothing else starts while it plays");
  stop_play_mode();
  t.check(!recorded_play_can_start(),
          "nor while the Stop is still being finished");
  // No pipeline frame runs here: end the session's log and finish the
  // Stop as the pipeline would.
  t.check(engine::runtime::end_play_log(), "the recording is saved");
  finish_play_stop();
  const std::string demo = std::string(directory) + "/take1.demo";
  std::error_code ec{};
  t.check(fs::exists(demo, ec) &&
              fs::exists(std::string(directory) + "/take1.input", ec),
          "it lands in the Recordings folder, manifest beside its log");
  t.check(!start_recorded_play("take1") &&
              (session.playState == PlayState::Stopped),
          "a recording is never replaced by another of its name");

  t.check(run_editor_action(EditorAction::ReplayLatest) &&
              (session.playState == PlayState::Playing) &&
              engine::runtime::play_replay_active(),
          "Replay Latest Recording enters Play replaying the newest");
  stop_play_mode();
  t.check(engine::runtime::end_play_log(), "the replay ends");
  finish_play_stop();

  // The console commands run the same paths.
  t.check(engine::core::console_execute("demorec take2") &&
              engine::runtime::play_recording_active(),
          "demorec <name> records");
  t.check(engine::core::console_execute("demostop") &&
              (session.playState == PlayState::Stopped),
          "demostop stops the session");
  t.check(engine::runtime::end_play_log(), "and it is saved");
  finish_play_stop();
  t.check(engine::core::console_execute("demoplay take1") &&
              engine::runtime::play_replay_active(),
          "demoplay <name> replays that recording");
  stop_play_mode();
  static_cast<void>(engine::runtime::end_play_log());
  finish_play_stop();
  t.check(engine::core::console_execute("demoplay missing") &&
              !engine::runtime::play_replay_active() &&
              (session.playState == PlayState::Stopped),
          "demoplay of a recording that does not exist starts nothing");

  fs::remove_all(directory, ec);
  session.playState = PlayState::Stopped;
  session.world = nullptr;
}

} // namespace

int main() {
  std::error_code ec{};
  fs::remove_all(kScratch, ec);
  fs::create_directories(kScratch, ec);
  if (ec || !engine::core::initialize_logging() ||
      !engine::core::initialize_cvars() ||
      !engine::core::initialize_console() ||
      !engine::core::set_project_data_root(kScratch) ||
      !register_recording_commands()) {
    return 1;
  }
  engine::tests::TestContext t;
  check_names(t);
  check_listing(t);
  check_record_and_replay(t);
  engine::core::clear_project_data_root();
  engine::core::shutdown_console();
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  fs::remove_all(kScratch, ec);
  return t.finish("editor_play_recording");
}
