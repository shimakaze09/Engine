// Implements the player's entry point: the executable that runs a game
// without the editor, as Unity's player build and Godot's export template
// do (docs/decisions/0016, point 4). It links engine_runtime and nothing
// from editor/, opens the project named on its command line (with none,
// the sample beside it), and plays its startup scene in a window titled
// with the project's name; --headless runs it without a window and
// --max-frames stops it after that many frames, for tests and servers. It
// prints to the terminal it was started from, if any, and says why in an
// error box when it cannot start. On the web it is the shared page, whose
// content is preloaded at the engine's default paths.

#include "engine/core/command_line.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/engine.h"
#include "engine/project.h"

#include <cstdint>
#include <cstdio>

namespace {

constexpr engine::core::CommandLineOption kOptions[] = {
    {"headless", engine::core::CommandLineOptionKind::Flag},
    {"max-frames", engine::core::CommandLineOptionKind::Value},
};

constexpr const char *kUsage =
    "Usage: engine_player [--headless] [--max-frames <count>] [<project "
    "directory or .project file>]";

/// Says why the player cannot start, on the terminal and in an error box,
/// naming the log when there is one, and returns the bootstrap failure
/// code.
int fail_to_start(const char *title, const char *message) noexcept {
  char text[1024] = {};
  const char *logPath = engine::core::log_file_path();
  std::snprintf(text, sizeof(text), "%s%s%s", message,
                (logPath[0] != '\0') ? "\n\nThe log says why:\n" : "", logPath);
  std::fprintf(stderr, "%s\n", text);
  engine::core::platform_show_error_box(title, text);
  return static_cast<int>(engine::ExitCode::BootstrapFailed);
}

/// Reads a frame count: a positive decimal that fits 32 bits, nothing
/// else. False for anything that is not one, which is refused rather than
/// read as 0 (run until quit).
bool parse_frame_count(const char *text, std::uint32_t *out) noexcept {
  if ((text == nullptr) || (text[0] == '\0')) {
    return false;
  }
  std::uint64_t value = 0U;
  for (const char *c = text; *c != '\0'; ++c) {
    if ((*c < '0') || (*c > '9')) {
      return false;
    }
    value = (value * 10U) + static_cast<std::uint64_t>(*c - '0');
    if (value > UINT32_MAX) {
      return false;
    }
  }
  if (value == 0U) {
    return false;
  }
  *out = static_cast<std::uint32_t>(value);
  return true;
}

} // namespace

/// Runs the player.
int main(int argc, char **argv) {
  static_cast<void>(engine::core::platform_attach_parent_console());

  const auto commandLine = engine::core::parse_command_line(
      argc, argv, kOptions, sizeof(kOptions) / sizeof(kOptions[0]), 1U);
  std::uint32_t maxFrames = 0U;
  const char *badArgument = nullptr;
  const char *reason = nullptr;
  if (!commandLine.has_value()) {
    const engine::core::CommandLineFailure failure = commandLine.error();
    badArgument = (failure.argumentIndex > 0) ? argv[failure.argumentIndex]
                                              : "engine_player";
    reason = engine::core::command_line_failure_text(failure.kind);
  } else if (commandLine->has("max-frames") &&
             !parse_frame_count(commandLine->value("max-frames"), &maxFrames)) {
    badArgument = "--max-frames";
    reason = "the frame count must be a positive whole number";
  }
  if (reason != nullptr) {
    char message[512] = {};
    std::snprintf(message, sizeof(message), "%s: %s\n\n%s", badArgument, reason,
                  kUsage);
    std::fprintf(stderr, "%s\n", message);
    engine::core::platform_show_error_box("Engine Player", message);
    return static_cast<int>(engine::ExitCode::BootstrapFailed);
  }

  // Static: about 18 KB, and the config points into it until bootstrap has
  // copied what it keeps.
  static engine::ProjectStorage project{};
  engine::EngineConfig config{};
  config.playerMode = true;
  config.core.platform.headless = commandLine->has("headless");
  char bundled[600] = {};
  const char *projectPath = commandLine->positional(0U);
  if ((projectPath == nullptr) &&
      engine::find_bundled_sample_project(bundled, sizeof(bundled))) {
    projectPath = bundled;
  }
  const char *title = "Engine Player";
  if (projectPath != nullptr) {
    const auto opened = engine::open_project(projectPath, &project, &config);
    if (!opened.has_value()) {
      char message[768] = {};
      std::snprintf(message, sizeof(message),
                    "The game could not be opened.\n\n%.400s: %s", projectPath,
                    engine::project_open_failure_text(opened.error().kind));
      return fail_to_start(title, message);
    }
    title = project.document.name;
  }
  config.core.platform.title = title;

  if (!engine::bootstrap(config)) {
    return fail_to_start(title, "The game could not start.");
  }
  char running[256] = {};
  std::snprintf(running, sizeof(running), "player: running '%s'", title);
  engine::core::log_message(engine::core::LogLevel::Info, "player", running);

  const engine::RunResult result = engine::run(maxFrames);
  engine::shutdown();
  return engine::run_result_exit_code(result);
}
