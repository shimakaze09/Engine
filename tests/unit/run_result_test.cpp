// Verifies engine::run failure propagation (issue #96): pipeline
// initialization failure must surface as a fatal run result that maps to a
// nonzero process exit code, while graceful stops map to zero.

#include "engine/engine.h"

#include <cstdio>

namespace {

/// Exit-code mapping contract: only Stopped may report shell success.
int check_exit_code_mapping() noexcept {
  if (engine::run_result_exit_code(engine::RunResult::Stopped) != 0) {
    return 10;
  }
  if (engine::run_result_exit_code(engine::RunResult::FatalInitialization) ==
      0) {
    return 11;
  }
  if (engine::run_result_exit_code(engine::RunResult::FatalFrame) == 0) {
    return 12;
  }
  return 0;
}

/// Injected init failure: running without bootstrap makes the production
/// EnginePipeline::initialize path fail (no core services, no VFS mount),
/// which engine::run must report as FatalInitialization, never Stopped.
int check_unbootstrapped_run_is_fatal() noexcept {
  const engine::RunResult result = engine::run(1U);
  if (result != engine::RunResult::FatalInitialization) {
    std::fprintf(stderr, "run without bootstrap returned %d\n",
                 static_cast<int>(result));
    return 20;
  }
  if (engine::run_result_exit_code(result) == 0) {
    return 21;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_exit_code_mapping();
  if (result != 0) {
    return result;
  }

  result = check_unbootstrapped_run_is_fatal();
  if (result != 0) {
    return result;
  }

  return 0;
}
