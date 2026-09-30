// A fatal ends the editor without losing the unsaved scene (#841, #1045).
// A fatal frame used to end engine::run with the World torn down and
// nothing saved, and a graphics-device fatal called abort(): either way
// the author's unsaved edits were gone.
//
// Drives engine::run, the production entry point, with the editor's
// session behind a bridge (no window) and a frame stage failed on purpose
// through dbg_fail_frame_stage:
//  - an unsaved scene is written to Recovery/ before the World goes, and
//    the run still reports FatalFrame;
//  - during Play the copy is the scene as it was before Play, not the
//    play world;
//  - a saved scene writes nothing.
// With the argument "device" it instead ends the process the way the
// renderer ends it on a device fatal, through core::terminate_after_fatal;
// run_fatal_device_recovery.cmake checks that process's exit code and the
// file it names.

#include "../asset_root.h"
#include "editor_recovery.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "engine/core/cvar.h"
#include "engine/core/fatal_exit.h"
#include "engine/core/project_data.h"
#include "engine/editor/editor.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

enum class Setup { UnsavedStopped, UnsavedPlaying, Saved };
Setup g_setup = Setup::UnsavedStopped;

/// Adds a named, transform-only entity to `world`.
bool add_named(engine::runtime::World &world, const char *name) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object();
  engine::runtime::NameComponent component{};
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  return (entity != engine::runtime::kInvalidEntity) &&
         world.add_name_component(entity, component);
}

/// The bridge's set_world: attaches the editor, then stages the case's
/// document before the first frame runs.
void attach_world(engine::runtime::World *world) noexcept {
  engine::editor::editor_set_world(world);
  if (world == nullptr) {
    return;
  }
  engine::editor::EditorSession &session = engine::editor::editor_session();
  session.initialized = true;
  CHECK(add_named(*world, "AuthoredEdit"), "author an edit");
  if (g_setup == Setup::UnsavedPlaying) {
    // Play snapshots the scene as authored; what the session then spawns
    // is play state, never part of the saved scene.
    engine::editor::start_play_mode();
    CHECK(session.hasPlaySnapshot, "Play took its snapshot");
    CHECK(add_named(*world, "PlayOnly"), "spawn during play");
  }
  session.document.unrecordedEdit = (g_setup != Setup::Saved);
}

engine::runtime::EditorBridge g_bridge{};

bool boot() noexcept {
  g_bridge.set_world = &attach_world;
  g_bridge.is_playing = &engine::editor::editor_is_playing;
  g_bridge.is_paused = &engine::editor::editor_is_paused;
  g_bridge.consume_play_transition = &engine::editor::consume_play_transition;
  g_bridge.complete_play_stop = &engine::editor::finish_play_stop;
  g_bridge.write_recovery_copy = &engine::editor::write_recovery_copy;
  engine::runtime::set_editor_bridge(&g_bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = 1U;
  if (!engine::bootstrap(config)) {
    return false;
  }
  // A project of the test's own, so its Recovery folder is private.
  std::error_code ec;
  std::filesystem::create_directories("fatal_recovery_test_project", ec);
  return engine::core::set_project_data_root("fatal_recovery_test_project");
}

/// The path the recovery note names, or "".
std::string recovery_path_from_note(const char *note) {
  const char *marker = "saved to:\n";
  const char *start = std::strstr(note, marker);
  if (start == nullptr) {
    return std::string();
  }
  start += std::strlen(marker);
  const char *end = std::strchr(start, '\n');
  return (end != nullptr) ? std::string(start, end) : std::string(start);
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

/// One run that fails its first frame; returns the recovery file's text
/// ("" when the note names none) after removing the file.
std::string run_fatal(Setup setup, engine::RunResult *outResult) {
  g_setup = setup;
  if (!boot()) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    ++g_failures;
    return std::string();
  }
  CHECK(
      engine::core::cvar_set_string("dbg_fail_frame_stage", "simulation_graph"),
      "arm the injected frame failure");
  *outResult = engine::run(10U);
  const std::string path =
      recovery_path_from_note(engine::fatal_recovery_note());
  engine::shutdown();
  if (path.empty()) {
    return std::string();
  }
  std::string text = read_file(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return text;
}

int run_device_child() {
  g_setup = Setup::UnsavedStopped;
  if (!boot()) {
    return 90;
  }
  // The pipeline attaches the World; a frame of it runs, then the device
  // fatal ends the process from inside the run, as the renderer does.
  engine::run(1U);
  // run() tore the World down; attach a fresh one the way the pipeline
  // would still hold it when a device fatal fires mid-frame.
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 91;
  }
  attach_world(world.get());
  engine::core::terminate_after_fatal("test: the graphics device was lost",
                                      engine::core::kFatalDeviceExitCode);
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: fixture setup\n");
    return 1;
  }
  if ((argc > 1) && (std::strcmp(argv[1], "device") == 0)) {
    return run_device_child();
  }

  engine::RunResult result = engine::RunResult::Stopped;
  const std::string stopped = run_fatal(Setup::UnsavedStopped, &result);
  CHECK(result == engine::RunResult::FatalFrame,
        "the injected failure still ends the run fatally");
  CHECK(stopped.find("\"AuthoredEdit\"") != std::string::npos,
        "the unsaved scene is written to Recovery/ before the World goes");

  const std::string playing = run_fatal(Setup::UnsavedPlaying, &result);
  CHECK(result == engine::RunResult::FatalFrame, "a fatal during Play");
  CHECK(playing.find("\"AuthoredEdit\"") != std::string::npos,
        "during Play the recovery copy holds the authored scene");
  CHECK(playing.find("\"PlayOnly\"") == std::string::npos,
        "during Play the recovery copy leaves out what Play spawned");

  const std::string saved = run_fatal(Setup::Saved, &result);
  CHECK(result == engine::RunResult::FatalFrame,
        "a fatal with nothing unsaved");
  CHECK(saved.empty() && (engine::fatal_recovery_note()[0] == '\0'),
        "a saved scene leaves no recovery copy and no note");

  engine::core::clear_project_data_root();
  if (g_failures != 0) {
    return 1;
  }
  std::printf("PASS: fatal runs keep the unsaved scene\n");
  return 0;
}
