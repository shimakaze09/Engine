// Implements main behavior for the Engine editor application entry point.

#include "engine/engine.h"

/// Runs this executable or test program.
int main() {
  if (!engine::bootstrap()) {
    return 1;
  }

  const engine::RunResult result = engine::run(0);
  engine::shutdown();
  return engine::run_result_exit_code(result);
}
