// Regression for #518: a world at entity capacity must survive a frame that
// runs every catch-up step the pacing clamp allows. The pipeline assembles
// one chunk-job table per kind for the whole frame and never resets the
// cursor between steps; at 65,536 transforms each step needs 256 chunks,
// so a 1024-entry table overflowed on the fifth step, the graph assembly
// reported failure and the run ended fatally — one long frame (asset load,
// window drag) on a large scene killed the process. Full production
// bootstrap, headless, on the null render device.

#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace {

engine::runtime::World *g_world = nullptr;

/// Captures the pipeline's world so the test can fill it before the frame.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }
bool bridge_is_playing() noexcept { return true; }
bool bridge_is_paused() noexcept { return false; }

/// Walks upward from the current path until the bundled assets are found.
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }
    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    return 2;
  }

  int result = 0;
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::shutdown();
      return 3;
    }

    // Fill the world to capacity with transformed entities: every one is
    // a chunk-job participant, so each fixed step needs the full chunk
    // count.
    while (g_world->create_scene_object() != engine::runtime::kInvalidEntity) {
    }
    const std::size_t transforms = g_world->transform_count();
    if (transforms < (engine::runtime::World::kMaxEntities - 64U)) {
      std::fprintf(stderr, "FAIL: only %zu transforms at capacity\n",
                   transforms);
      result = 4;
    }

    // Sleep past every step the clamp allows (8 x 16.67 ms), so this one
    // frame simulates the maximum catch-up. The sleep forces the step
    // count; nothing about wall-clock time is asserted.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const bool frameRan = pipeline.execute_frame();
    if (!frameRan || pipeline.had_fatal_error()) {
      std::fprintf(stderr,
                   "FAIL: a full world's catch-up frame ended the run "
                   "(ran=%d fatal=%d)\n",
                   frameRan ? 1 : 0, pipeline.had_fatal_error() ? 1 : 0);
      result = 5;
    }
    // And the run keeps going afterwards.
    if ((result == 0) && !pipeline.execute_frame()) {
      std::fprintf(stderr, "FAIL: the frame after catch-up did not run\n");
      result = 6;
    }
    pipeline.teardown();
  }

  engine::shutdown();
  if (result == 0) {
    std::puts("pipeline_full_world_catchup_test passed");
  }
  return result;
}
