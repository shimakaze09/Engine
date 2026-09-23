// Helper for crash_report_test.cmake: bootstraps the engine, runs frames
// so the frame index and the stage table are live, then faults on purpose
// inside a known stage. The parent reads stderr and checks the report
// names the build, the frame, the stage and the thread.
//
// The fault has to happen while a stage is running, not between frames --
// reporting "between-frames" would not distinguish a working stage
// publication from no publication at all. A log sink is the seam: sinks
// run inside log_message, so a sink that faults on the "slice" channel
// faults inside stage_diagnostics, which is the stage that logs it. The
// report must then name that stage and a non-zero frame.
//
// argv[1] selects what to do, so one binary covers every case the driver
// needs:
//   report   write a report directly, without faulting
//   segv     fault inside the diagnostics stage

#include "engine/core/crash_report.h"
#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"

#include <cstdio>
#include <cstring>

namespace {

bool g_armed = false;

/// Raises the fault from inside a stage. Reached through the log sink,
/// which the scripting stage drives when a script logs.
void fault_now() noexcept {
  // Volatile so the store is not optimised away, and through a null
  // pointer so this is a genuine SIGSEGV rather than a raise() that could
  // pass while real faults did not.
  volatile int *target = nullptr;
  *target = 1;
}

void arming_sink(engine::core::LogLevel /*level*/, const char *channel,
                 const char * /*message*/, void * /*userData*/) noexcept {
  if (!g_armed || (channel == nullptr)) {
    return;
  }
  // "slice" is emitted by stage_diagnostics, and only once a frame has
  // been counted, so both facts the report prints are non-trivial.
  if ((std::strcmp(channel, "slice") == 0) &&
      (engine::core::log_current_frame_index() >= 1U)) {
    g_armed = false;
    fault_now();
  }
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  const char *mode = (argc > 1) ? argv[1] : "report";

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = 1U;
  if (!engine::bootstrap(config)) {
    std::printf("HELPER-FAILED bootstrap\n");
    std::fflush(stdout);
    return 1;
  }

  if (std::strcmp(mode, "report") == 0) {
    // No fault: proves the report's content independently of whether a
    // signal handler was reached, so a failure in the driver's other case
    // can be told apart from a failure to format.
    engine::core::write_crash_report(2, "requested");
    engine::shutdown();
    return 0;
  }

  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U)) {
    std::printf("HELPER-FAILED pipeline\n");
    std::fflush(stdout);
    engine::shutdown();
    return 1;
  }
  static_cast<void>(pipeline.set_frame_delta_override(1.0 / 60.0));

  if (!engine::core::log_register_sink(&arming_sink, nullptr)) {
    std::printf("HELPER-FAILED sink\n");
    std::fflush(stdout);
    pipeline.teardown();
    engine::shutdown();
    return 1;
  }

  // Everything buffered so far is flushed before the fault, so the parent
  // can tell a helper that never got here from one whose report is
  // missing.
  std::printf("HELPER-READY\n");
  std::fflush(stdout);

  // Enough frames that stage_diagnostics logs its periodic line at least
  // once past frame 0; the fault ends the loop long before the bound.
  g_armed = true;
  for (int i = 0; i < 240; ++i) {
    static_cast<void>(pipeline.execute_frame());
  }

  // Only reached if the sink never fired.
  std::printf("HELPER-FAILED no fault\n");
  std::fflush(stdout);
  pipeline.teardown();
  engine::shutdown();
  return 1;
}
