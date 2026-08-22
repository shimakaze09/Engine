// Acceptance smoke for #221: an authored orthographic camera drives real
// rendered frames end to end — authored through the production World API
// in Input phase, published by the pipeline's camera stage, and read back
// orthographic from the renderer while frames produce draw calls. The
// CPU-verifiable projection/cascade/culling proofs live in the unit
// suites, and the Lua binding path is pinned by scripting_test's camera
// block; this test pins the live wiring on a real GL device.

#include "engine/engine.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

engine::runtime::World *g_world = nullptr;

/// Captures the pipeline's world so the test can author entities between frames.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool bridge_is_playing() noexcept { return true; }
bool bridge_is_paused() noexcept { return false; }

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Walks upward from the current path until the bundled assets are found
/// (same technique as pipeline_tick_cadence_test.cpp).
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

/// Runs one playing frame guaranteed to simulate at least one fixed step
/// (see pipeline_tick_cadence_test.cpp on the wall-clock accumulator).
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

  if (!engine::bootstrap()) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    return 2;
  }

  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U) || (g_world == nullptr)) {
    std::fprintf(stderr, "FAIL: pipeline initialize\n");
    pipeline.teardown();
    engine::shutdown();
    return 3;
  }

  CHECK(ticking_frame(pipeline), "settle frame 1");
  CHECK(ticking_frame(pipeline), "settle frame 2");
  // Author the orthographic camera between frames (Input phase) through
  // the production World API; the Lua binding path is pinned headlessly by
  // scripting_test's camera block.
  const engine::runtime::Entity camEntity = g_world->create_scene_object();
  CHECK(camEntity != engine::runtime::kInvalidEntity, "spawn camera entity");
  engine::runtime::CameraComponent camera{};
  camera.projection = static_cast<std::uint32_t>(
      engine::runtime::CameraProjection::Orthographic);
  camera.orthographicSize = 9.0F;
  camera.priority = 99.0F;
  camera.blendSpeed = 100.0F;
  CHECK(g_world->add_camera_component(camEntity, camera), "author camera");
  CHECK(ticking_frame(pipeline), "publish frame 1");
  CHECK(ticking_frame(pipeline), "publish frame 2");
  CHECK(ticking_frame(pipeline), "rendered frame");

  // The authored ortho camera won the priority stack and reached the
  // renderer with its kind and half-height intact.
  const engine::renderer::CameraState active =
      engine::renderer::get_active_camera();
  CHECK(active.projection ==
            engine::renderer::CameraState::kProjectionOrthographic,
        "active camera is orthographic");
  CHECK(active.orthographicSize == 9.0F,
        "orthographic half-height propagated");

  // Frames actually render under the ortho projection.
  CHECK(engine::renderer::renderer_get_last_frame_stats().drawCalls > 0U,
        "ortho frames produce draw calls");

  pipeline.teardown();
  engine::shutdown();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("ortho_camera_render_test passed");
  return 0;
}
