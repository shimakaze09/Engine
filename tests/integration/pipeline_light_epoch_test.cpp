// Regression for #569 row 3: one renderer submission must come from one
// mutation epoch. Render prep builds the draw list before stage_post_frame
// dispatches collision callbacks, end-play and the deferred-mutation
// flush, and stage_render used to collect lights and captures after that,
// so a light entity a collision handler destroyed was drawn but not lit
// in the same submission. Lights are now collected right after render
// prep; this test observes the count the pipeline publishes in
// EngineStats. Full production bootstrap, headless, on the null render
// device; the contact is a resting sphere overlapping a static block under
// zero gravity so the pair is reported on the first physics step.

#include "engine/core/engine_stats.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

constexpr const char *kScriptPath = "pipeline_light_epoch_test.lua";
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

// The handler runs in stage_post_frame, after render prep built the draw
// list with the light alive; the destroy is applied before stage_render.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    engine.on_collision_handler(function(a, b)\n"
    "        local light = engine.find_entity_by_name('EpochLight')\n"
    "        if light then engine.destroy_entity(light) end\n"
    "    end)\n"
    "end\n"
    "return M\n";

/// Authors the resting contact pair that fires the collision handler.
bool author_contact_pair(engine::runtime::World &world) noexcept {
  engine::runtime::set_gravity(world, 0.0F, 0.0F, 0.0F);
  const engine::runtime::Entity block =
      world.create_scene_object(engine::runtime::Transform{});
  engine::runtime::Transform sphereTransform{};
  sphereTransform.position = engine::math::Vec3(0.0F, 0.9F, 0.0F);
  const engine::runtime::Entity sphere =
      world.create_scene_object(sphereTransform);
  if ((block == engine::runtime::kInvalidEntity) ||
      (sphere == engine::runtime::kInvalidEntity)) {
    return false;
  }
  engine::runtime::Collider blockCollider{};
  blockCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::Collider sphereCollider{};
  sphereCollider.shape = engine::runtime::ColliderShape::Sphere;
  sphereCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  return world.add_collider(block, blockCollider) &&
         world.add_collider(sphere, sphereCollider) &&
         world.add_rigid_body(sphere, body);
}

bool write_script_file() noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(kScript);
  const bool ok = std::fwrite(kScript, 1U, length, file) == length;
  return (std::fclose(file) == 0) && ok;
}

/// Runs one playing frame guaranteed to simulate at least one fixed step.
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return pipeline.execute_frame();
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets() || !write_script_file()) {
    std::fprintf(stderr, "FAIL: assets or script file\n");
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

    const engine::runtime::Entity light = g_world->create_scene_object();
    engine::runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "EpochLight");
    engine::runtime::PointLightComponent point{};
    const engine::runtime::Entity scripted = g_world->create_scene_object();
    engine::runtime::ScriptComponent script{};
    std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s",
                  kScriptPath);
    if ((light == engine::runtime::kInvalidEntity) ||
        !g_world->add_name_component(light, name) ||
        !g_world->add_point_light_component(light, point) ||
        (scripted == engine::runtime::kInvalidEntity) ||
        !g_world->add_script_component(scripted, script) ||
        !author_contact_pair(*g_world)) {
      std::fprintf(stderr, "FAIL: authoring\n");
      result = 4;
    }

    // Every frame up to the one whose post-frame handler destroys the
    // light built its draw list with the light alive, so every one of
    // them must also have lit it. The frame after lights nothing.
    constexpr int kMaxFrames = 10;
    int destroyFrame = -1;
    for (int frame = 1; (result == 0) && (frame <= kMaxFrames); ++frame) {
      if (!ticking_frame(pipeline)) {
        std::fprintf(stderr, "FAIL: frame %d\n", frame);
        result = 5;
        break;
      }
      const std::uint32_t lit = engine::core::get_engine_stats().sceneLights;
      const bool alive = g_world->is_alive(light);
      if (destroyFrame < 0) {
        if (lit != 1U) {
          std::fprintf(stderr,
                       "FAIL: frame %d built its draw list with the light "
                       "alive but submitted %u lights\n",
                       frame, lit);
          result = alive ? 6 : 7;
          break;
        }
        if (!alive) {
          destroyFrame = frame;
        }
      } else {
        if (lit != 0U) {
          std::fprintf(stderr, "FAIL: frame %d lit %u lights after the "
                               "destroy\n",
                       frame, lit);
          result = 8;
        }
        break;
      }
    }
    if ((result == 0) && (destroyFrame < 0)) {
      std::fprintf(stderr, "FAIL: the collision handler never destroyed the "
                           "light within %d frames\n",
                   kMaxFrames);
      result = 9;
    }
    pipeline.teardown();
  }

  engine::shutdown();
  std::remove(kScriptPath);
  if (result == 0) {
    std::puts("pipeline_light_epoch_test passed");
  }
  return result;
}
