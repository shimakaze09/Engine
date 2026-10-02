// Implements the Engine editor application's entry point (the game runs
// without it in engine_player, player/main.cpp): a windowed
// application (no console window on Windows) that prints to the
// terminal it was started from, if any, and says why in an error box when
// it cannot start, since it has no console to say it in. It opens the
// project named on its command line (a directory or a .project document);
// with none, the project hub, where a project is chosen or created. A run
// ends by quitting or by switching projects (File > Open Project, Close
// Project, or a choice in the hub): the engine then shuts down and boots
// again with the next project, or the hub, as Godot's editor relaunches
// on a project chosen in its project manager. The web page has no
// filesystem to choose from: it runs once, on the content preloaded at
// the engine's default paths.

#include "engine/core/command_line.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/engine.h"
#include "engine/project.h"

#include <cstdio>
#include <cstring>

namespace {

/// Shows `message` in an error box and returns the bootstrap failure code.
int fail_to_start(const char *message) noexcept {
  char text[1024] = {};
  const char *logPath = engine::core::log_file_path();
  std::snprintf(text, sizeof(text), "%s%s%s", message,
                (logPath[0] != '\0') ? "\n\nThe log says why:\n" : "", logPath);
  // A terminal it was started from sees it too; the box waits for a click.
  std::fprintf(stderr, "%s\n", text);
  engine::core::platform_show_error_box("Engine", text);
  return static_cast<int>(engine::ExitCode::BootstrapFailed);
}

/// Says that the run stopped on a fatal frame error, where the unsaved
/// scene was saved, if it was, and where the log is. Called before
/// shutdown, while the log file is still open.
void report_fatal_frame() noexcept {
  char text[1400] = {};
  const char *note = engine::fatal_recovery_note();
  const char *logPath = engine::core::log_file_path();
  std::snprintf(text, sizeof(text),
                "The editor stopped on an internal error.%s%s%s%s",
                (note[0] != '\0') ? "\n\n" : "", note,
                (logPath[0] != '\0') ? "\n\nThe log says why:\n" : "", logPath);
  std::fprintf(stderr, "%s\n", text);
  engine::core::platform_show_error_box("Engine", text);
}

/// Shows why the project at `path` did not open and returns the bootstrap
/// failure code.
int fail_to_open(const char *path,
                 engine::ProjectOpenFailureKind kind) noexcept {
  char message[768] = {};
  std::snprintf(message, sizeof(message),
                "The project could not be opened.\n\n%.400s: %s", path,
                engine::project_open_failure_text(kind));
  return fail_to_start(message);
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  static_cast<void>(engine::core::platform_attach_parent_console());

  const auto commandLine =
      engine::core::parse_command_line(argc, argv, nullptr, 0U, 1U);
  if (!commandLine.has_value()) {
    char message[512] = {};
    const engine::core::CommandLineFailure failure = commandLine.error();
    std::snprintf(message, sizeof(message),
                  "%s: %s\n\nUsage: engine_editor_app [<project directory or "
                  ".project file>]",
                  (failure.argumentIndex > 0) ? argv[failure.argumentIndex]
                                              : "engine_editor_app",
                  engine::core::command_line_failure_text(failure.kind));
    std::fprintf(stderr, "%s\n", message);
    engine::core::platform_show_error_box("Engine", message);
    return static_cast<int>(engine::ExitCode::BootstrapFailed);
  }

  // Where the OS owns the main loop (the browser), run() hands the frames
  // to it and never returns, so there is one session: no hub and no
  // project switch.
  if (engine::core::platform_caps().ownsMainLoop) {
    engine::EngineConfig config{};
    if (!engine::bootstrap(config)) {
      return fail_to_start("The editor could not start.");
    }
    const engine::RunResult hostedResult = engine::run(0);
    engine::shutdown();
    return engine::run_result_exit_code(hostedResult);
  }

  // Static: about 18 KB, and the config points into it until bootstrap has
  // copied what it keeps.
  static engine::ProjectStorage project{};
  // The project the next run opens; empty is the hub.
  char next[engine::kProjectOsPathCapacity] = {};
  const char *named = commandLine->positional(0U);
  if (named != nullptr) {
    if (std::strlen(named) >= sizeof(next)) {
      return fail_to_open(named, engine::ProjectOpenFailureKind::PathTooLong);
    }
    std::snprintf(next, sizeof(next), "%s", named);
  }
  bool fromCommandLine = (named != nullptr);

  for (;;) {
    engine::EngineConfig config{};
    if (next[0] == '\0') {
      engine::configure_without_project(&config);
    } else {
      const auto opened = engine::open_project(next, &project, &config);
      if (!opened.has_value()) {
        // The project the user named at launch is the one they wanted, so
        // it fails the start; one chosen in the hub or a menu was checked
        // when chosen, and one that has gone since goes back to the hub.
        if (fromCommandLine) {
          return fail_to_open(next, opened.error().kind);
        }
        engine::configure_without_project(&config);
      }
    }
    fromCommandLine = false;

    if (!engine::bootstrap(config)) {
      return fail_to_start("The editor could not start.");
    }
    const engine::RunResult result = engine::run(0);
    if (result == engine::RunResult::FatalFrame) {
      report_fatal_frame();
    }
    engine::shutdown();
    if (result != engine::RunResult::Stopped) {
      return engine::run_result_exit_code(result);
    }
    bool toHub = false;
    if (!engine::take_project_switch(next, sizeof(next), &toHub)) {
      return engine::run_result_exit_code(result);
    }
  }
}
