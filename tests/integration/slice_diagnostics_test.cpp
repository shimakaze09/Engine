// Verifies the pipeline's slice diagnostics line is an engine diagnostic,
// not a user message: it logs at Trace, which the editor Console hides by
// default, once every 60 frames, and never at a higher level. The run
// boots the empty startup scene headless at a fixed 60 Hz frame delta, so
// frames 0, 60 and 120 of 121 carry the line and no others do.

#include "../asset_root.h"
#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;
int g_traceLines = 0;
int g_otherLevelLines = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Counts the "slice" channel's lines by level.
void count_slice_lines(engine::core::LogLevel level, const char *channel,
                       const char *, void *) noexcept {
  if ((channel == nullptr) || (std::strcmp(channel, "slice") != 0)) {
    return;
  }
  if (level == engine::core::LogLevel::Trace) {
    ++g_traceLines;
  } else {
    ++g_otherLevelLines;
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    return 2;
  }
  const bool sinkOk =
      engine::core::log_register_sink(&count_slice_lines, nullptr);
  CHECK(sinkOk, "register the log sink");

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::shutdown();
      return 3;
    }
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0),
          "fix the frame delta");
    for (int frame = 0; frame <= 120; ++frame) {
      CHECK(pipeline.execute_frame(), "frame");
    }
    pipeline.teardown();
  }

  if (sinkOk) {
    engine::core::log_unregister_sink(&count_slice_lines, nullptr);
  }
  engine::shutdown();

  CHECK(g_otherLevelLines == 0, "no slice line logs above Trace");
  if (g_traceLines != 3) {
    std::fprintf(stderr,
                 "FAIL: %d slice lines in 121 frames, expected 3 (frames "
                 "0, 60 and 120)\n",
                 g_traceLines);
    ++g_failures;
  }
  if (g_failures != 0) {
    std::fprintf(stderr, "slice_diagnostics_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("slice_diagnostics_test: all checks passed\n");
  return 0;
}
