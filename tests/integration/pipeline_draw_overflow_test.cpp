// Regression for #519: more visible draws than a command buffer holds is a
// per-frame degradation — the overflow is dropped, counted and reported —
// never a fatal run exit. Render prep treated a full buffer as a
// frame-graph failure and the pipeline treated that as fatal, so a scene
// past kMaxDrawCommands visible meshes ended the process under a log line
// that said only "entity dropped from frame". Full production bootstrap,
// headless, on the null render device.

#include "engine/core/engine_stats.h"
#include "engine/engine.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace {

engine::runtime::World *g_world = nullptr;

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

/// Runs one playing frame guaranteed to simulate at least one fixed step.
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return pipeline.execute_frame();
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
    // Settle so the bootstrap scene's meshes are loaded and drawing. The
    // null device counts no draw calls, so the drop count asserted below
    // is also the proof that the clones were visible draws.
    if (!ticking_frame(pipeline) || !ticking_frame(pipeline)) {
      result = 4;
    }

    // Clone a visible mesh entity — same asset, same transform — past the
    // command-buffer capacity, so every clone is a visible draw.
    engine::runtime::Entity source = engine::runtime::kInvalidEntity;
    engine::runtime::MeshComponent mesh{};
    g_world->for_each<engine::runtime::MeshComponent>(
        [&](engine::runtime::Entity entity,
            const engine::runtime::MeshComponent &component) {
          if (source == engine::runtime::kInvalidEntity) {
            source = entity;
            mesh = component;
          }
        });
    engine::runtime::Transform transform{};
    if ((result == 0) &&
        ((source == engine::runtime::kInvalidEntity) ||
         !g_world->get_transform(source, &transform))) {
      std::fprintf(stderr, "FAIL: no mesh entity to clone\n");
      result = 6;
    }
    constexpr std::size_t kOverflow = 1024U;
    const std::size_t clones =
        engine::renderer::CommandBufferBuilder::kMaxDrawCommands + kOverflow;
    for (std::size_t i = 0U; (result == 0) && (i < clones); ++i) {
      const engine::runtime::Entity clone =
          g_world->create_scene_object(transform);
      if ((clone == engine::runtime::kInvalidEntity) ||
          !g_world->add_mesh_component(clone, mesh)) {
        std::fprintf(stderr, "FAIL: clone %zu\n", i);
        result = 7;
      }
    }

    if (result == 0) {
      const bool frameRan = ticking_frame(pipeline);
      const engine::core::EngineStats stats = engine::core::get_engine_stats();
      if (!frameRan || pipeline.had_fatal_error()) {
        std::fprintf(stderr,
                     "FAIL: draw overflow ended the run (ran=%d fatal=%d)\n",
                     frameRan ? 1 : 0, pipeline.had_fatal_error() ? 1 : 0);
        result = 8;
      } else if (stats.droppedDrawCommands < kOverflow) {
        std::fprintf(stderr, "FAIL: %u draws reported dropped, at least %zu "
                             "expected\n",
                     stats.droppedDrawCommands, kOverflow);
        result = 9;
      } else if (!pipeline.execute_frame()) {
        std::fprintf(stderr, "FAIL: the frame after the overflow did not run\n");
        result = 10;
      }
    }
    pipeline.teardown();
  }

  engine::shutdown();
  if (result == 0) {
    std::puts("pipeline_draw_overflow_test passed");
  }
  return result;
}
