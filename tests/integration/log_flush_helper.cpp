// Helper for log_flush_on_error_test.cmake: logs one Error line, then one
// Info line, then leaves via std::_Exit, which runs no atexit handler and
// flushes nothing. Whatever reaches the parent's pipe is therefore exactly
// what the logging layer chose to flush itself.
//
// Order matters. The Error comes first so the Info behind it is still
// sitting in the buffer at exit: the Error must arrive and the Info must
// not, which distinguishes a selective flush both from no flush at all and
// from an unbuffered stream that would deliver everything regardless.

#include "engine/core/logging.h"

#include <cstdio>
#include <cstdlib>

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging()) {
    // The parent greps stdout, so a setup failure has to be visible there
    // too rather than only in the exit code.
    std::printf("HELPER-FAILED initialize_logging\n");
    std::fflush(stdout);
    return 1;
  }

  engine::core::log_message(engine::core::LogLevel::Error, "flushtest",
                            "error-line-must-survive");
  engine::core::log_message(engine::core::LogLevel::Info, "flushtest",
                            "info-line-must-not-survive");

  // Dies with a full buffer, the way a crash does.
  std::_Exit(0);
}
