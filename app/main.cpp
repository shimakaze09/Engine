// Implements the Engine editor application's entry point: a windowed
// application (no console window on Windows) that prints to the
// terminal it was started from, if any, and says why in an error box when
// it cannot start, since it has no console to say it in.

#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/engine.h"

#include <cstdio>

/// Runs this executable or test program.
int main() {
  static_cast<void>(engine::core::platform_attach_parent_console());
  if (!engine::bootstrap()) {
    char message[768] = {};
    const char *logPath = engine::core::log_file_path();
    std::snprintf(message, sizeof(message), "The editor could not start.%s%s",
                  (logPath[0] != '\0') ? "\n\nThe log says why:\n" : "",
                  logPath);
    engine::core::platform_show_error_box("Engine", message);
    return static_cast<int>(engine::ExitCode::BootstrapFailed);
  }

  const engine::RunResult result = engine::run(0);
  engine::shutdown();
  return engine::run_result_exit_code(result);
}
