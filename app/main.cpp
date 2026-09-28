// Implements the Engine editor application's entry point: a windowed
// application (no console window on Windows) that prints to the
// terminal it was started from, if any, and says why in an error box when
// it cannot start, since it has no console to say it in. It opens the
// project named on its command line (a directory or a .project document);
// with none, the sample project beside the executable, until the project
// hub takes that role; and where there is none either (the web page, whose
// content is preloaded at assets/), the engine's default content paths.

#include "engine/core/command_line.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"
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

/// The sample project beside the executable, into `out`; false when there
/// is none.
bool find_bundled_project(char *out, std::size_t capacity) noexcept {
  char appDir[512] = {};
  if (!engine::core::platform_get_app_dir(appDir, sizeof(appDir))) {
    return false;
  }
  const std::size_t length = std::strlen(appDir);
  const bool slash = (length > 0U) && ((appDir[length - 1U] == '/') ||
                                       (appDir[length - 1U] == '\\'));
  const int written = std::snprintf(out, capacity, "%s%ssamples/island", appDir,
                                    slash ? "" : "/");
  return (written > 0) && (static_cast<std::size_t>(written) < capacity) &&
         engine::core::os_directory_exists(out);
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

  // Static: about 18 KB, and the config points into it until bootstrap has
  // copied what it keeps.
  static engine::ProjectStorage project{};
  engine::EngineConfig config{};
  char bundled[600] = {};
  const char *projectPath = commandLine->positional(0U);
  if ((projectPath == nullptr) &&
      find_bundled_project(bundled, sizeof(bundled))) {
    projectPath = bundled;
  }
  if (projectPath != nullptr) {
    const auto opened = engine::open_project(projectPath, &project, &config);
    if (!opened.has_value()) {
      char message[768] = {};
      std::snprintf(message, sizeof(message),
                    "The project could not be opened.\n\n%.400s: %s",
                    projectPath,
                    engine::project_open_failure_text(opened.error().kind));
      return fail_to_start(message);
    }
  }

  if (!engine::bootstrap(config)) {
    return fail_to_start("The editor could not start.");
  }

  const engine::RunResult result = engine::run(0);
  engine::shutdown();
  return engine::run_result_exit_code(result);
}
