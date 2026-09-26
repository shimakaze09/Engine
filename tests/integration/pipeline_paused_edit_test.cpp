// Verifies that a paused frame prepares what it presents from the current
// World: drives the production EnginePipeline with a bridge that plays and
// then pauses. While paused, a mesh is moved from behind the camera to in
// front of it, and the camera entity is moved, the way the editor's live
// edit writes the World between frames. The next paused frame must
// propagate the moved transform, evaluate the edited camera and prepare
// the draws, with no simulation step: paused frames that edit nothing
// leave the World's state hash unchanged.

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/simulation_clock.h"
#include "engine/engine.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace {

engine::runtime::World *g_world = nullptr;
bool g_playing = true;
bool g_paused = false;

void capture_world(engine::runtime::World *world) noexcept { g_world = world; }
bool bridge_playing() noexcept { return g_playing; }
bool bridge_paused() noexcept { return g_paused; }

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
        std::filesystem::exists(
            normalized / "assets/shaders/bgfx/shaders.manifest", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// Copies the first mesh asset id found in the bootstrap scene.
std::uint64_t
find_any_mesh_asset_id(const engine::runtime::World &world) noexcept {
  std::uint64_t assetId = 0ULL;
  world.for_each<engine::runtime::MeshComponent>(
      [&assetId](engine::runtime::Entity,
                 const engine::runtime::MeshComponent &mesh) {
        if (assetId == 0ULL) {
          assetId = mesh.meshAssetId;
        }
      });
  return assetId;
}

/// Adds a mesh entity at `position`; kInvalidEntity on failure.
engine::runtime::Entity add_mesh(engine::runtime::World &world,
                                 std::uint64_t meshAssetId,
                                 const engine::math::Vec3 &position) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = meshAssetId;
  return ((entity != engine::runtime::kInvalidEntity) &&
          world.add_mesh_component(entity, mesh))
             ? entity
             : engine::runtime::kInvalidEntity;
}

/// Draw commands render prep kept for the main camera last frame.
std::uint32_t draw_calls() noexcept {
  return engine::core::get_engine_stats().drawCommands;
}

int run(engine::EnginePipeline &pipeline) noexcept {
  const std::uint64_t meshAssetId = find_any_mesh_asset_id(*g_world);
  if (meshAssetId == 0ULL) {
    return 5;
  }
  engine::runtime::reset_world(*g_world);

  // The camera entity at the origin looks down -Z; the mesh starts
  // behind it.
  const engine::runtime::Entity meshA =
      add_mesh(*g_world, meshAssetId, engine::math::Vec3(0.0F, 0.0F, 60.0F));
  const engine::runtime::Entity cameraEntity = g_world->create_scene_object();
  engine::runtime::CameraComponent camera{};
  camera.priority = 10.0F;
  camera.farPlane = 200.0F;
  // A saturated blend weight: the view snaps to the entity's pose, so the
  // assertion below reads the pose itself rather than a smoothing step.
  camera.blendSpeed = 1000.0F;
  if ((meshA == engine::runtime::kInvalidEntity) ||
      (cameraEntity == engine::runtime::kInvalidEntity) ||
      !g_world->add_camera_component(cameraEntity, camera)) {
    return 6;
  }

  // Each playing frame runs exactly one fixed step, so the camera's blend
  // has saturated before the pause.
  pipeline.set_frame_delta_override(engine::core::kFixedDeltaSeconds);
  for (int frame = 0; frame < 6; ++frame) {
    if (!pipeline.execute_frame()) {
      return 7;
    }
  }
  const std::uint32_t drawsNone = draw_calls();

  // Pause, and let one paused frame settle.
  g_playing = false;
  g_paused = true;
  if (!pipeline.execute_frame()) {
    return 8;
  }
  if (draw_calls() != drawsNone) {
    std::fprintf(stderr, "FAIL: pausing alone changed the draws (%u -> %u)\n",
                 drawsNone, draw_calls());
    return 9;
  }

  // Live edit: move mesh A in front of the camera.
  engine::runtime::Transform moved{};
  moved.position = engine::math::Vec3(0.0F, 0.0F, -30.0F);
  if (!g_world->add_transform(meshA, moved) || !pipeline.execute_frame()) {
    return 10;
  }
  const engine::runtime::WorldTransform *world =
      g_world->get_world_transform_read_ptr(meshA);
  if ((world == nullptr) || (world->position.z != -30.0F)) {
    std::fprintf(stderr, "FAIL: the paused frame did not propagate the edit\n");
    return 11;
  }
  const std::uint32_t drawsMoved = draw_calls();
  if (engine::core::get_engine_stats().fixedSteps != 0U) {
    std::fprintf(stderr, "FAIL: a paused frame ran a simulation step\n");
    return 17;
  }
  if (drawsMoved <= drawsNone) {
    std::fprintf(stderr,
                 "FAIL: the paused frame did not draw the moved mesh "
                 "(%u, was %u)\n",
                 drawsMoved, drawsNone);
    return 12;
  }

  // Frames that edit nothing change nothing: no step runs while paused.
  const std::uint64_t hashAfterEdit = g_world->state_hash();
  for (int frame = 0; frame < 3; ++frame) {
    if (!pipeline.execute_frame()) {
      return 13;
    }
  }
  if ((g_world->state_hash() != hashAfterEdit) ||
      (draw_calls() != drawsMoved)) {
    std::fprintf(stderr, "FAIL: idle paused frames changed the World or "
                         "the draws\n");
    return 14;
  }

  // Live edit: move the camera entity back along +Z.
  engine::runtime::Transform cameraMoved{};
  cameraMoved.position = engine::math::Vec3(0.0F, 0.0F, 5.0F);
  if (!g_world->add_transform(cameraEntity, cameraMoved) ||
      !pipeline.execute_frame()) {
    return 15;
  }
  if (engine::renderer::get_active_camera().position.z != 5.0F) {
    std::fprintf(stderr, "FAIL: the paused frame did not evaluate the edited "
                         "camera\n");
    return 16;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    return 1;
  }
  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_playing;
  bridge.is_paused = &bridge_paused;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    return 2;
  }
  static_cast<void>(engine::core::cvar_set_int("r_max_fps", 30));

  int result = 0;
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      result = 3;
    } else if (g_world == nullptr) {
      result = 4;
    } else {
      result = run(pipeline);
    }
    pipeline.teardown();
  }
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();
  if (result == 0) {
    std::printf("pipeline_paused_edit_test: all checks passed\n");
  } else {
    std::fprintf(stderr, "pipeline_paused_edit_test: failed with %d\n", result);
  }
  return result;
}
