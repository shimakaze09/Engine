// Regression for #168 M4 (core-tier lifecycle hygiene, plus #235/#236):
// initialize_core must own the cvar and console tables so a repeated
// bootstrap in one process starts from defaults (before this fix nothing in
// production ever initialized or cleared them — registrations and user-set
// values leaked across shutdown_core → initialize_core, and the console
// built-ins help/set/get were never registered in a shipped run), and
// shutdown_logging must drop leaked sink registrations so a dead sink is
// never dispatched to after re-initialization.

#include "engine/core/bootstrap.h"
#include "engine/core/console.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

engine::core::CoreConfig headless_config() noexcept {
  engine::core::CoreConfig config{};
  config.frameAllocatorBytes = 1024U * 1024U;
  config.initializePlatform = false;
  return config;
}

/// CVar registrations and values must not leak across core lifecycles.
void check_cvar_lifecycle() {
  CHECK(engine::core::initialize_core(headless_config()), "core init 1");
  CHECK(engine::core::cvar_register_int("test_m4_cvar", 5, "M4 test cvar"),
        "first registration succeeds");
  CHECK(engine::core::cvar_set_int("test_m4_cvar", 9), "set to non-default");
  CHECK(engine::core::cvar_get_int("test_m4_cvar") == 9, "value stuck");
  engine::core::shutdown_core();

  // A second core lifecycle registers the same name fresh and re-applies
  // the default — the #168 step-7 restart boundary.
  CHECK(engine::core::initialize_core(headless_config()), "core init 2");
  CHECK(engine::core::cvar_register_int("test_m4_cvar", 5, "M4 test cvar"),
        "re-registration succeeds after shutdown_core");
  CHECK(engine::core::cvar_get_int("test_m4_cvar", -1) == 5,
        "re-registration restores the default value");
  engine::core::shutdown_core();
}

/// The console built-ins must be live in a production core session and act
/// on the production cvar table.
void check_console_builtins() {
  CHECK(engine::core::initialize_core(headless_config()), "core init");
  CHECK(engine::core::console_execute("help"),
        "help is registered by core initialization");
  CHECK(engine::core::cvar_register_int("test_m4_console", 1, "M4 console"),
        "register console-target cvar");
  CHECK(engine::core::console_execute("set test_m4_console 7"),
        "console set executes");
  CHECK(engine::core::cvar_get_int("test_m4_console") == 7,
        "console set reached the cvar table");
  engine::core::shutdown_core();
  CHECK(!engine::core::console_execute("help"),
        "console table is empty after shutdown_core");
}

int g_sinkCalls = 0;

/// Counts dispatches so the test can prove a leaked sink is dropped.
void counting_sink(engine::core::LogLevel /*level*/, const char * /*channel*/,
                   const char * /*message*/, void *userData) noexcept {
  ++(*static_cast<int *>(userData));
}

/// A sink whose owner forgot to unregister must not survive a logging
/// lifecycle (#236).
void check_logging_sink_dropped() {
  CHECK(engine::core::initialize_logging(), "logging init");
  CHECK(engine::core::log_register_sink(&counting_sink, &g_sinkCalls),
        "register sink");
  engine::core::log_message(engine::core::LogLevel::Info, "test", "one");
  CHECK(g_sinkCalls == 1, "sink sees traffic while registered");

  engine::core::shutdown_logging();
  CHECK(engine::core::initialize_logging(), "logging re-init");
  engine::core::log_message(engine::core::LogLevel::Info, "test", "two");
  CHECK(g_sinkCalls == 1, "leaked sink is not dispatched after re-init");
  engine::core::shutdown_logging();
}

} // namespace

/// Runs this executable or test program.
int main() {
  check_cvar_lifecycle();
  check_console_builtins();
  check_logging_sink_dropped();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("core_lifecycle_test passed");
  return 0;
}
