// Regression for #765: a Reflection Probe could be added, configured and
// saved, and nothing in the frame read it, so a placed probe changed
// nothing. The pipeline now hands every probe to the renderer each frame,
// render prep keeps the draws a probe's capture reaches, the Game view's
// flush captures and bakes the probe, and a view whose camera is inside
// the probe's box is lit by it. Full production bootstrap, headless, on
// the null render device.

#include "../builtin_mesh_fixture.h"

#include "../asset_root.h"
#include "engine/core/engine_stats.h"
#include "engine/engine.h"
#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

engine::runtime::World *g_world = nullptr;

void capture_world(engine::runtime::World *world) noexcept { g_world = world; }
bool bridge_is_playing() noexcept { return true; }
bool bridge_is_paused() noexcept { return false; }

/// Runs one playing frame guaranteed to simulate at least one fixed step.
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return pipeline.execute_frame();
}

bool same_point(const engine::math::Vec3 &a,
                const engine::math::Vec3 &b) noexcept {
  return (a.x == b.x) && (a.y == b.y) && (a.z == b.z);
}

/// Runs frames until the probe at request 0 has been baked `bakes` times;
/// probes bake one per frame, so a few frames always suffice.
bool bake_settles(engine::EnginePipeline &pipeline, std::uint32_t bakes,
                  engine::renderer::ReflectionProbeStatus *out) noexcept {
  for (int frame = 0; frame < 4; ++frame) {
    if (!ticking_frame(pipeline) ||
        !engine::renderer::get_reflection_probe_status(0U, out)) {
      return false;
    }
    if (out->bakeCount >= bakes) {
      return true;
    }
  }
  return false;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: assets\n");
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
    if ((engine::tests::add_builtin_cube(*g_world, engine::math::Vec3()) ==
         engine::runtime::kInvalidEntity) ||
        !ticking_frame(pipeline) || !ticking_frame(pipeline)) {
      result = 4;
    }

    // A cube behind the camera: the main camera culls it.
    const engine::renderer::CameraState cam =
        engine::renderer::get_active_camera();
    const engine::math::Vec3 forward =
        engine::math::normalize(engine::math::sub(cam.target, cam.position));
    const engine::math::Vec3 behind =
        engine::math::sub(cam.position, engine::math::mul(forward, 12.0F));
    if ((result == 0) && ((engine::tests::add_builtin_cube(*g_world, behind) ==
                           engine::runtime::kInvalidEntity) ||
                          !ticking_frame(pipeline))) {
      result = 5;
    }
    const engine::core::EngineStats before = engine::core::get_engine_stats();
    if ((result == 0) &&
        ((engine::renderer::reflection_probe_request_count() != 0U) ||
         (before.reflectionProbeDraws != 0U) ||
         (engine::renderer::active_reflection_probe(
              engine::renderer::RenderViewId::Game) != -1))) {
      std::fprintf(stderr, "FAIL: probe state without a probe\n");
      result = 6;
    }

    // A probe midway between the camera and the hidden cube, its box
    // around the camera and its capture reaching the cube.
    const engine::math::Vec3 probeAt =
        engine::math::sub(cam.position, engine::math::mul(forward, 6.0F));
    engine::runtime::Transform probeTransform{};
    probeTransform.position = probeAt;
    engine::runtime::ReflectionProbeComponent probe{};
    probe.boxExtents = engine::math::Vec3(20.0F, 20.0F, 20.0F);
    probe.radius = 15.0F;
    probe.prefilteredResolution = 64U;
    probe.mipLevels = 3U;
    probe.irradianceResolution = 16U;
    const engine::runtime::Entity probeEntity =
        (result == 0) ? g_world->create_scene_object(probeTransform)
                      : engine::runtime::kInvalidEntity;
    engine::renderer::ReflectionProbeStatus status{};
    if ((result == 0) &&
        ((probeEntity == engine::runtime::kInvalidEntity) ||
         !g_world->add_reflection_probe_component(probeEntity, probe) ||
         !bake_settles(pipeline, 1U, &status))) {
      std::fprintf(stderr, "FAIL: the probe was never baked\n");
      result = 7;
    }
    if ((result == 0) &&
        ((engine::renderer::reflection_probe_request_count() != 1U) ||
         !same_point(status.capturePosition, probeAt) ||
         (status.faceSize != 64U) || (status.mipLevels != 3U) ||
         (status.irradianceFaceSize != 16U) || !status.baked)) {
      std::fprintf(stderr, "FAIL: the probe was baked with other settings\n");
      result = 8;
    }
    const engine::core::EngineStats withProbe =
        engine::core::get_engine_stats();
    if ((result == 0) && (withProbe.reflectionProbeDraws != 1U)) {
      std::fprintf(stderr,
                   "FAIL: the draw only the probe sees was not kept for it "
                   "(%u)\n",
                   withProbe.reflectionProbeDraws);
      result = 9;
    }
    if ((result == 0) && (withProbe.captureOnlyDraws != 0U)) {
      std::fprintf(stderr, "FAIL: a probe draw counted as a capture draw\n");
      result = 10;
    }
    if ((result == 0) && (engine::renderer::active_reflection_probe(
                              engine::renderer::RenderViewId::Game) != 0)) {
      std::fprintf(stderr,
                   "FAIL: the camera inside the probe's box is not lit by "
                   "it\n");
      result = 11;
    }

    // An unchanged probe is not captured again; a moved one is, from where
    // it now stands.
    if (result == 0) {
      const std::uint32_t bakes = status.bakeCount;
      if (!ticking_frame(pipeline) || !ticking_frame(pipeline) ||
          !engine::renderer::get_reflection_probe_status(0U, &status) ||
          (status.bakeCount != bakes)) {
        std::fprintf(stderr, "FAIL: an unchanged probe was captured again\n");
        result = 12;
      }
    }
    const engine::math::Vec3 movedTo =
        engine::math::add(probeAt, engine::math::Vec3(0.0F, 1.0F, 0.0F));
    if (result == 0) {
      engine::runtime::Transform moved = probeTransform;
      moved.position = movedTo;
      const std::uint32_t bakes = status.bakeCount;
      if (!g_world->add_transform(probeEntity, moved) ||
          !bake_settles(pipeline, bakes + 1U, &status) ||
          !same_point(status.capturePosition, movedTo)) {
        std::fprintf(stderr, "FAIL: a moved probe was not captured again\n");
        result = 13;
      }
    }

    // A camera outside every box is lit by the sky.
    if (result == 0) {
      engine::runtime::ReflectionProbeComponent small = probe;
      small.boxExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
      if (!g_world->add_reflection_probe_component(probeEntity, small) ||
          !bake_settles(pipeline, status.bakeCount + 1U, &status) ||
          (engine::renderer::active_reflection_probe(
               engine::renderer::RenderViewId::Game) != -1)) {
        std::fprintf(stderr,
                     "FAIL: a camera outside the probe's box is lit by it\n");
        result = 14;
      }
    }

    // Removing the probe removes its request.
    if (result == 0) {
      if (!g_world->remove_reflection_probe_component(probeEntity) ||
          !ticking_frame(pipeline) ||
          (engine::renderer::reflection_probe_request_count() != 0U)) {
        std::fprintf(stderr, "FAIL: a removed probe is still requested\n");
        result = 15;
      }
    }
    pipeline.teardown();
  }

  engine::shutdown();
  if (result == 0) {
    std::puts("pipeline_reflection_probe_test passed");
  }
  return result;
}
