// Verifies the GPU profiler on a device that offers no timestamp
// queries: it degrades to zero timings rather than failing, and it
// reports the absence exactly once at Info. The level and the count are
// the contract, not decoration — this fires on a supported backend, so
// at Warning or Error it reads as a fault the user should act on, and
// per frame it drowns the log.

#include "engine/renderer/gpu_profiler.h"

#include <cstring>

#include "engine/core/logging.h"

namespace {

/// What the log sink saw about timestamp queries.
struct TimestampMessages final {
  int atInfo = 0;
  int aboveInfo = 0;
};

TimestampMessages g_messages;

/// Counts only the timestamp-query line, by level, so an unrelated line
/// from another subsystem cannot satisfy or break the assertion.
void count_sink(engine::core::LogLevel level, const char *channel,
                const char *message, void *userData) noexcept {
  static_cast<void>(channel);
  static_cast<void>(userData);
  if ((message == nullptr) ||
      (std::strstr(message, "timestamp queries") == nullptr)) {
    return;
  }
  if (level == engine::core::LogLevel::Info) {
    ++g_messages.atInfo;
  } else if (level > engine::core::LogLevel::Info) {
    ++g_messages.aboveInfo;
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  using namespace engine::renderer;

  if (!engine::core::initialize_logging()) {
    return 1;
  }
  if (!engine::core::log_register_sink(&count_sink, nullptr)) {
    engine::core::shutdown_logging();
    return 2;
  }
  const auto finish = [](int code) noexcept {
    engine::core::log_unregister_sink(&count_sink, nullptr);
    engine::core::shutdown_logging();
    return code;
  };

  // Without a render device, initialization degrades gracefully and
  // leaves timings at 0.
  if (!initialize_gpu_profiler()) {
    return finish(3);
  }

  gpu_profiler_begin_frame();
  gpu_profiler_begin_pass(GpuPassId::Scene);
  gpu_profiler_end_pass(GpuPassId::Scene);
  gpu_profiler_begin_pass(GpuPassId::Tonemap);
  gpu_profiler_end_pass(GpuPassId::Tonemap);

  const float sceneMs = gpu_profiler_pass_ms(GpuPassId::Scene);
  const float tonemapMs = gpu_profiler_pass_ms(GpuPassId::Tonemap);
  if ((sceneMs < 0.0F) || (tonemapMs < 0.0F)) {
    shutdown_gpu_profiler();
    return finish(4);
  }

  const GpuProfilerDebugStats debugStats = gpu_profiler_debug_stats();
  if ((debugStats.beginMarksScene == 0U) || (debugStats.endMarksScene == 0U) ||
      (debugStats.beginMarksTonemap == 0U) ||
      (debugStats.endMarksTonemap == 0U)) {
    shutdown_gpu_profiler();
    return finish(5);
  }

  // Said once, and at Info. A repeat initialization must not say it
  // again, and neither must a frame: the absence is a property of the
  // device, so it is news exactly once.
  if (g_messages.atInfo != 1) {
    shutdown_gpu_profiler();
    return finish(6);
  }
  if (g_messages.aboveInfo != 0) {
    shutdown_gpu_profiler();
    return finish(7);
  }

  if (!initialize_gpu_profiler()) {
    shutdown_gpu_profiler();
    return finish(8);
  }
  for (int i = 0; i < 8; ++i) {
    gpu_profiler_begin_frame();
    gpu_profiler_begin_pass(GpuPassId::Scene);
    gpu_profiler_end_pass(GpuPassId::Scene);
  }
  if ((g_messages.atInfo != 1) || (g_messages.aboveInfo != 0)) {
    shutdown_gpu_profiler();
    return finish(9);
  }

  shutdown_gpu_profiler();
  return finish(0);
}
