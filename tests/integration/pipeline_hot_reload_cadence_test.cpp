// Regression for #528: the pipeline polled every watched shader and script
// timestamp on every frame, in every mode. Hot-reload polling is editor
// work: a run without an editor bridge never polls, and a run with one
// polls on its first frame and then on a coarse interval. Full production
// bootstrap, headless, on the null render device; observed through
// EngineStats.hotReloadPolls. The interval itself is wall-clock and is
// deliberately not asserted.

#include "../asset_root.h"
#include "engine/core/engine_stats.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"

#include <cstdio>
#include <filesystem>

namespace {

bool bridge_is_playing() noexcept { return false; }
bool bridge_is_paused() noexcept { return false; }

/// Boots the engine, runs `frames` frames, and reports the polls of the
/// first frame and of all frames; negative on a bootstrap failure.
int run_frames(int frames, std::uint32_t *outFirst,
               std::uint32_t *outTotal) noexcept {
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    return -1;
  }
  int result = 0;
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      pipeline.teardown();
      engine::shutdown();
      return -2;
    }
    *outFirst = 0U;
    *outTotal = 0U;
    for (int frame = 0; frame < frames; ++frame) {
      if (!pipeline.execute_frame()) {
        result = -3;
        break;
      }
      const std::uint32_t polls = engine::core::get_engine_stats().hotReloadPolls;
      if (frame == 0) {
        *outFirst = polls;
      }
      *outTotal += polls;
    }
    pipeline.teardown();
  }
  engine::shutdown();
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: assets\n");
    return 1;
  }

  // A player (no editor bridge) never polls.
  engine::runtime::set_editor_bridge(nullptr);
  std::uint32_t first = 0U;
  std::uint32_t total = 0U;
  int result = run_frames(4, &first, &total);
  if (result != 0) {
    std::fprintf(stderr, "FAIL: player run (%d)\n", result);
    return 2;
  }
  if (total != 0U) {
    std::fprintf(stderr, "FAIL: a player run polled for hot reload %u times\n",
                 total);
    return 3;
  }

  // An editor polls on its first frame, once.
  engine::runtime::EditorBridge bridge{};
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);
  result = run_frames(1, &first, &total);
  engine::runtime::set_editor_bridge(nullptr);
  if (result != 0) {
    std::fprintf(stderr, "FAIL: editor run (%d)\n", result);
    return 4;
  }
  if (first != 1U) {
    std::fprintf(stderr, "FAIL: the first editor frame polled %u times\n",
                 first);
    return 5;
  }
  std::puts("pipeline_hot_reload_cadence_test passed");
  return 0;
}
