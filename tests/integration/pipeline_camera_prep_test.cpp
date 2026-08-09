// Regression for issue #78: render prep must cull with the camera published
// this frame (between the last fixed step and render prep), not the camera
// from the previous frame. Drives the production EnginePipeline frame loop:
// with a single mesh entity and a camera-manager camera, the frame that moves
// the camera onto the mesh must draw it that same frame.

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/engine.h"
#include "engine/math/transform.h"
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

/// Captures the pipeline's world so the test can author entities between frames.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

/// Reports the play state as always playing so the camera stage runs.
bool always_playing() noexcept { return true; }

/// Reports the pause state as never paused so the frame graph runs.
bool never_paused() noexcept { return false; }

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
        std::filesystem::exists(normalized / "assets/shaders/default.vert",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
}

/// Copies the first mesh asset id found in the bootstrap scene.
std::uint64_t find_any_mesh_asset_id(
    const engine::runtime::World &world) noexcept {
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

/// Updates the single test camera with the given pose (instant: the entry's
/// saturated blend weight makes evaluate snap to the new pose).
bool push_test_camera(engine::runtime::World &world,
                      engine::runtime::Entity owner,
                      const engine::math::Vec3 &position,
                      const engine::math::Vec3 &target) noexcept {
  engine::runtime::CameraEntry entry{};
  entry.position = position;
  entry.target = target;
  entry.up = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  entry.blendSpeed = 1000.0F;
  entry.nearPlane = 0.1F;
  entry.farPlane = 200.0F;
  return world.camera_manager().push_camera(owner, entry, 10.0F);
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &always_playing;
  bridge.is_paused = &never_paused;
  engine::runtime::set_editor_bridge(&bridge);

  if (!engine::bootstrap()) {
    return 2;
  }

  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U)) {
    pipeline.teardown();
    engine::shutdown();
    return 3;
  }

  if (g_world == nullptr) {
    pipeline.teardown();
    engine::shutdown();
    return 4;
  }

  static_cast<void>(engine::core::cvar_set_int("r_max_fps", 30));

  const std::uint64_t meshAssetId = find_any_mesh_asset_id(*g_world);
  if (meshAssetId == 0ULL) {
    pipeline.teardown();
    engine::shutdown();
    return 5;
  }

  engine::runtime::reset_world(*g_world);

  engine::runtime::Transform targetTransform{};
  targetTransform.position = engine::math::Vec3(0.0F, 0.0F, -60.0F);
  const engine::runtime::Entity target =
      g_world->create_scene_object(targetTransform);
  if (target == engine::runtime::kInvalidEntity) {
    pipeline.teardown();
    engine::shutdown();
    return 6;
  }

  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = meshAssetId;
  if (!g_world->add_mesh_component(target, mesh)) {
    pipeline.teardown();
    engine::shutdown();
    return 7;
  }

  const engine::runtime::Entity cameraOwner = g_world->create_scene_object();
  if (cameraOwner == engine::runtime::kInvalidEntity) {
    pipeline.teardown();
    engine::shutdown();
    return 8;
  }

  const engine::math::Vec3 awayPos(0.0F, 0.0F, 60.0F);
  const engine::math::Vec3 awayTarget(0.0F, 0.0F, 160.0F);
  const engine::math::Vec3 facingPos(0.0F, 0.0F, -10.0F);
  const engine::math::Vec3 facingTarget(0.0F, 0.0F, -60.0F);

  if (!push_test_camera(*g_world, cameraOwner, awayPos, awayTarget)) {
    pipeline.teardown();
    engine::shutdown();
    return 9;
  }

  constexpr int kWarmupFrames = 12;
  for (int frame = 0; frame < kWarmupFrames; ++frame) {
    if (!pipeline.execute_frame()) {
      pipeline.teardown();
      engine::shutdown();
      return 10;
    }
  }
  const std::uint32_t drawsAway = engine::core::get_engine_stats().drawCalls;

  if (!push_test_camera(*g_world, cameraOwner, facingPos, facingTarget)) {
    pipeline.teardown();
    engine::shutdown();
    return 11;
  }

  if (!pipeline.execute_frame()) {
    pipeline.teardown();
    engine::shutdown();
    return 12;
  }
  const std::uint32_t drawsMoveFrame = engine::core::get_engine_stats().drawCalls;

  if (!pipeline.execute_frame()) {
    pipeline.teardown();
    engine::shutdown();
    return 13;
  }
  const std::uint32_t drawsFacing = engine::core::get_engine_stats().drawCalls;

  pipeline.teardown();
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();

  if (drawsFacing <= drawsAway) {
    std::fprintf(stderr,
                 "pipeline_camera_prep_test: camera move never revealed the "
                 "mesh (away=%u facing=%u)\n",
                 drawsAway, drawsFacing);
    return 14;
  }

  if (drawsMoveFrame != drawsFacing) {
    std::fprintf(stderr,
                 "pipeline_camera_prep_test: render prep culled with a stale "
                 "camera on the move frame (move=%u facing=%u away=%u)\n",
                 drawsMoveFrame, drawsFacing, drawsAway);
    return 15;
  }

  std::printf("pipeline_camera_prep_test: all tests passed\n");
  return 0;
}
