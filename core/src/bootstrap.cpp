// Implements bootstrap behavior for the Engine core engine.

#include "engine/core/bootstrap.h"

#include <cstddef>
#include <cstdio>
#include <thread>

#include "engine/core/console.h"
#include "engine/core/cvar.h"
#include "engine/core/debug_draw.h"
#include "engine/core/engine_stats.h"
#include "engine/core/engine_version.h"
#include "engine/core/event_bus.h"
#include "engine/core/input.h"
#include "engine/core/job_system.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/profiler.h"
#include "engine/core/project_data.h"
#include "engine/core/reflect.h"
#include "engine/core/thread_affinity.h"
#include "engine/core/vfs.h"

namespace engine::core {

namespace {

bool g_coreInitialized = false;

} // namespace

/// Initializes the owning system for core with explicit subsystem ownership.
bool initialize_core(const CoreConfig &config) noexcept {
  if (g_coreInitialized) {
    return true;
  }
  // Whoever initializes core owns the main thread: the platform queue, the
  // renderer and the Lua VM all run where this call ran.
  set_main_thread();

  bool loggingInitialized = false;
  bool cvarsInitialized = false;
  bool consoleInitialized = false;
  bool debugDrawInitialized = false;
  bool vfsInitialized = false;
  bool eventBusInitialized = false;
  bool platformInitialized = false;
  bool inputInitialized = false;
  bool profilerInitialized = false;
  bool jobSystemInitialized = false;
  bool initializedSuccessfully = false;
  const char *failureMessage = nullptr;

  do {
    if (!initialize_logging()) {
      failureMessage = "failed to initialize logging";
      break;
    }
    loggingInitialized = true;

    // Static REFLECT_TYPE blocks ran before any log existed; this is the
    // first point their refused registrations can be reported. A dropped
    // type or field is a programmer error in the schema's capacity, not a
    // recoverable runtime condition, so initialization continues.
    static_cast<void>(report_reflection_registration_drops());

    // Core owns the cvar/console tables: production registration no
    // longer relies on the zero-initialized fallback, and shutdown_core
    // clears both so a later bootstrap starts from defaults.
    if (!initialize_cvars()) {
      failureMessage = "failed to initialize cvars";
      break;
    }
    cvarsInitialized = true;

    if (!initialize_console()) {
      failureMessage = "failed to initialize console";
      break;
    }
    consoleInitialized = true;

    if (!initialize_vfs()) {
      failureMessage = "failed to initialize virtual file system";
      break;
    }
    vfsInitialized = true;

    if (!initialize_event_bus()) {
      failureMessage = "failed to initialize event bus";
      break;
    }
    eventBusInitialized = true;

    if (config.initializePlatform) {
      if (!initialize_platform(config.platform)) {
        failureMessage = "failed to initialize platform";
        break;
      }
      platformInitialized = true;
    }

    // Before input, which restores the project's saved bindings. The
    // project's GUID names it when there is one, so its data survives a
    // move; otherwise the content root does. A root that cannot be
    // resolved is logged and leaves that data refused; it does not stop
    // the engine.
    if (asset_guid_is_valid(config.projectGuid)) {
      static_cast<void>(set_project_data_guid(config.projectGuid));
    } else if (config.projectRoot != nullptr) {
      static_cast<void>(set_project_data_root(config.projectRoot));
    }

    if (!initialize_input()) {
      failureMessage = "failed to initialize input";
      break;
    }
    inputInitialized = true;

    if (!initialize_profiler()) {
      failureMessage = "failed to initialize profiler";
      break;
    }
    profilerInitialized = true;

    // Fixed-storage queue whose init cannot fail today; tracked anyway so
    // the failure rollback below stays symmetric with shutdown_core.
    debugDrawInitialized = initialize_debug_draw();

    const std::uint32_t hardwareThreads = std::thread::hardware_concurrency();
    const std::uint32_t workerThreads =
        (config.workerThreads > 0U)
            ? config.workerThreads
            : ((hardwareThreads > 1U) ? (hardwareThreads - 1U) : 0U);
    if (!initialize_job_system(workerThreads)) {
      failureMessage = "failed to initialize job system";
      break;
    }
    jobSystemInitialized = true;

    initializedSuccessfully = true;
  } while (false);

  if (initializedSuccessfully) {
    g_coreInitialized = true;
    // The build identity, logged once as its own line: every log file and
    // every pasted excerpt then names the binary it came from, including
    // whether the tree was dirty and which float flags were in force, so
    // a report never has to be traced back by asking.
    log_message(LogLevel::Info, "core", engine_build_id());
    log_message(LogLevel::Info, "core", "core initialized");
    return true;
  }

  if (loggingInitialized && (failureMessage != nullptr)) {
    log_message(LogLevel::Error, "core", failureMessage);
  }

  if (jobSystemInitialized) {
    shutdown_job_system();
  }

  if (debugDrawInitialized) {
    shutdown_debug_draw();
  }

  if (profilerInitialized) {
    shutdown_profiler();
  }

  if (inputInitialized) {
    shutdown_input();
  }

  if (platformInitialized) {
    shutdown_platform();
  }

  if (eventBusInitialized) {
    shutdown_event_bus();
  }

  if (vfsInitialized) {
    shutdown_vfs();
  }

  if (consoleInitialized) {
    shutdown_console();
  }

  if (cvarsInitialized) {
    shutdown_cvars();
  }

  if (loggingInitialized) {
    shutdown_logging();
  }

  clear_project_data_root();
  clear_main_thread();

  return false;
}

/// Shuts down the owning system for core.
void shutdown_core() noexcept {
  if (!g_coreInitialized) {
    return;
  }
  clear_main_thread();

  shutdown_job_system();
  shutdown_debug_draw();
  shutdown_profiler();
  shutdown_input();
  shutdown_platform();
  clear_project_data_root();
  shutdown_event_bus();
  shutdown_vfs();
  shutdown_console();
  shutdown_cvars();
  shutdown_logging();

  // The published stats snapshot is run-scoped; a later core in this
  // process must not read the previous run's numbers before its own.
  reset_engine_stats();
  g_coreInitialized = false;
}

/// Returns whether is core initialized.
bool is_core_initialized() noexcept { return g_coreInitialized; }

} // namespace engine::core
