// Implements assertion_failed: the one place a broken contract is
// reported, so the message shape is the same wherever ENGINE_ASSERT is
// used.

#include "engine/core/assertion.h"

#include "engine/core/logging.h"

#include <cstdio>
#include <cstdlib>

namespace engine::core {

void assertion_failed(const char *condition, const char *file,
                      int line) noexcept {
  char message[512] = {};
  std::snprintf(message, sizeof(message), "%s:%d: assertion failed: %s",
                (file != nullptr) ? file : "?", line,
                (condition != nullptr) ? condition : "?");

  // stderr first, and unconditionally. log_message drops everything
  // before initialize_logging and after shutdown_logging, which are
  // exactly the moments a contract is most likely to break -- a bounds
  // check that fires during startup would otherwise abort in silence.
  // Nothing about reporting a broken invariant should depend on another
  // subsystem being up.
  std::fprintf(stderr, "%s\n", message);
  std::fflush(stderr);

  // Then through logging as well, so an editor-side capture records the
  // line that is about to end the process. On an initialized build this
  // prints the message a second time, which is the right trade on a path
  // that ends the process: a duplicate costs a line, a miss costs the
  // diagnostic.
  log_message(LogLevel::Fatal, "assert", message);

  // log_message(Fatal) aborts when logging is up, and does nothing at all
  // when it is not, so this is the abort in that case rather than dead
  // code.
  std::abort();
}

} // namespace engine::core
