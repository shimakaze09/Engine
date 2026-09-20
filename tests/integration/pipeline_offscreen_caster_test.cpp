// Regression for #524: render prep culled every draw against the main
// camera and the shadow and capture passes consumed that same list, so a
// caster just outside the view cast no shadow and a scene capture looking
// away from the player captured nothing. Render prep now keeps a
// camera-culled draw in the auxiliary list when sweeping it along the
// directional light reaches the view, or a capture camera sees it, and
// the pipeline reports both counts. Full production bootstrap, headless,
// on the null render device; observed through EngineStats.

#include "engine/core/engine_stats.h"
#include "engine/engine.h"
#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"
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
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.manifest",
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

/// Points the bootstrap scene's directional light along `direction`.
bool set_sun_direction(const engine::math::Vec3 &direction) noexcept {
  using namespace engine::runtime;
  Entity sun = kInvalidEntity;
  LightComponent light{};
  g_world->for_each<LightComponent>(
      [&](Entity entity, const LightComponent &component) {
        if ((sun == kInvalidEntity) &&
            (component.type == LightType::Directional)) {
          sun = entity;
          light = component;
        }
      });
  if (sun == kInvalidEntity) {
    return false;
  }
  light.direction = direction;
  return g_world->add_light_component(sun, light);
}

struct Counts final {
  std::uint32_t draws = 0U;
  std::uint32_t casters = 0U;
  std::uint32_t captureOnly = 0U;
};

bool frame_counts(engine::EnginePipeline &pipeline, Counts *out) noexcept {
  if (!ticking_frame(pipeline)) {
    return false;
  }
  const engine::core::EngineStats stats = engine::core::get_engine_stats();
  out->draws = stats.drawCommands;
  out->casters = stats.offscreenShadowCasters;
  out->captureOnly = stats.captureOnlyDraws;
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
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
    // Settle so the bootstrap meshes are loaded and the camera is final.
    if (!ticking_frame(pipeline) || !ticking_frame(pipeline)) {
      result = 4;
    }

    // Borrow a visible mesh for the caster, and put it behind the camera:
    // outside the frustum, but with the sun shining from behind the camera
    // its shadow falls into the view.
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
    const engine::renderer::CameraState cam =
        engine::renderer::get_active_camera();
    const engine::math::Vec3 forward =
        engine::math::normalize(engine::math::sub(cam.target, cam.position));
    engine::runtime::Transform behind{};
    behind.position =
        engine::math::sub(cam.position, engine::math::mul(forward, 12.0F));
    Counts baseline{};
    if ((result == 0) &&
        ((source == engine::runtime::kInvalidEntity) ||
         !set_sun_direction(forward) || !frame_counts(pipeline, &baseline))) {
      std::fprintf(stderr, "FAIL: baseline\n");
      result = 5;
    }

    engine::runtime::Entity caster = engine::runtime::kInvalidEntity;
    Counts withCaster{};
    if (result == 0) {
      caster = g_world->create_scene_object(behind);
      if ((caster == engine::runtime::kInvalidEntity) ||
          !g_world->add_mesh_component(caster, mesh) ||
          !frame_counts(pipeline, &withCaster)) {
        std::fprintf(stderr, "FAIL: caster authoring\n");
        result = 6;
      } else if (withCaster.draws != baseline.draws) {
        std::fprintf(stderr, "FAIL: the caster behind the camera changed the "
                             "main draw list (%u -> %u)\n",
                     baseline.draws, withCaster.draws);
        result = 7;
      } else if (withCaster.casters != baseline.casters + 1U) {
        std::fprintf(stderr,
                     "FAIL: the off-screen caster was not kept for the shadow "
                     "passes (%u -> %u)\n",
                     baseline.casters, withCaster.casters);
        result = 8;
      }
    }

    // With the sun reversed the caster's shadow falls away from the view,
    // so it is no longer kept.
    Counts reversed{};
    if (result == 0) {
      if (!set_sun_direction(engine::math::mul(forward, -1.0F)) ||
          !frame_counts(pipeline, &reversed)) {
        result = 9;
      } else if (reversed.casters != withCaster.casters - 1U) {
        std::fprintf(stderr,
                     "FAIL: a caster whose shadow leaves the view was kept "
                     "(%u vs %u)\n",
                     reversed.casters, withCaster.casters);
        result = 10;
      }
    }

    // A scene capture looking at the caster keeps it for that capture
    // even though neither the camera nor the sun wants it.
    if (result == 0) {
      engine::runtime::Transform captureAt{};
      captureAt.position =
          engine::math::add(behind.position, engine::math::Vec3(0.0F, 0.0F, 5.0F));
      const engine::runtime::Entity capture =
          g_world->create_scene_object(captureAt);
      engine::runtime::SceneCaptureComponent component{};
      Counts withCapture{};
      if ((capture == engine::runtime::kInvalidEntity) ||
          !g_world->add_scene_capture_component(capture, component) ||
          !frame_counts(pipeline, &withCapture)) {
        result = 11;
      } else if (withCapture.captureOnly != reversed.captureOnly + 1U) {
        std::fprintf(stderr,
                     "FAIL: the draw only the capture camera sees was not "
                     "kept for it (%u -> %u)\n",
                     reversed.captureOnly, withCapture.captureOnly);
        result = 12;
      } else if (withCapture.draws != baseline.draws) {
        result = 13;
      }
    }
    pipeline.teardown();
  }

  engine::shutdown();
  if (result == 0) {
    std::puts("pipeline_offscreen_caster_test passed");
  }
  return result;
}
