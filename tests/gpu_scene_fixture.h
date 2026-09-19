// Boots the production engine on the real render device for a
// gpu-labelled test and hands the test a pipeline and an empty World to
// author a scene into. Every pixel-readback test needs the same bring-up —
// the editor bridge that captures the World, the asset working directory,
// the cvars that make a frame repeatable, the skip when no real device is
// present — so it lives here once and a test file holds only its scene and
// what it expects of the image.

#pragma once

#include "gpu_frame_capture.h"

#include "engine/content/asset_metadata.h"
#include "engine/core/cvar.h"
#include "engine/engine.h"
#include "engine/math/transform.h"
#include "engine/renderer/render_device.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace engine::tests {

/// The body of a GPU scene test: authors a scene, captures frames, and
/// returns the process exit code (0 passes). Printing a line that starts
/// with "SKIPPED:" and returning 0 reports a stated skip to ctest.
using GpuSceneBody = int (*)(engine::EnginePipeline &pipeline,
                             engine::runtime::World &world);

/// Authors the scene the pipeline's very first frame draws. Most tests
/// build theirs in the body, after that frame; a test about what the first
/// frame itself leaves behind passes one of these instead. False fails the
/// test.
using GpuFirstFrameScene = bool (*)(engine::runtime::World &world);

namespace detail {

inline engine::runtime::World *g_fixtureWorld = nullptr;
inline bool g_cvarRefused = false;

inline void capture_world(engine::runtime::World *world) noexcept {
  g_fixtureWorld = world;
}
inline bool always_playing() noexcept { return true; }
inline bool never_paused() noexcept { return false; }

/// Walks upward from the current path until the bundled assets are found.
inline bool enter_asset_directory() noexcept {
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

} // namespace detail

/// Records the outcome of a cvar set made by a test body. A refused set —
/// a misspelt name, or one not registered yet — would leave the test
/// running in a configuration it does not describe, so the fixture fails
/// the test when any was refused, whatever the body returns.
inline void checked(bool accepted, const char *what) noexcept {
  if (!accepted) {
    std::fprintf(stderr, "FAIL: cvar set refused: %s\n", what);
    detail::g_cvarRefused = true;
  }
}

/// Adds a scene object drawing one of the built-in meshes ("builtin://cube",
/// "builtin://plane", ...). kInvalidEntity when the World refuses either
/// step.
inline engine::runtime::Entity
add_builtin_mesh(engine::runtime::World &world, const char *builtinPath,
                 const engine::runtime::Transform &transform,
                 const engine::math::Vec3 &albedo) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = engine::content::make_asset_id_from_path(builtinPath);
  mesh.albedo = albedo;
  mesh.roughness = 0.9F;
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world.add_mesh_component(entity, mesh)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

/// Points the view at target from position. The blend is saturated at
/// once, so the first frames already show this pose rather than easing
/// toward it.
inline bool look_from(engine::runtime::World &world,
                      const engine::math::Vec3 &position,
                      const engine::math::Vec3 &target) noexcept {
  const engine::runtime::Entity owner = world.create_scene_object();
  if (owner == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::CameraEntry entry{};
  entry.position = position;
  entry.target = target;
  entry.up = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  entry.blendSpeed = 1000.0F;
  entry.nearPlane = 0.1F;
  entry.farPlane = 200.0F;
  return world.camera_manager().push_camera(owner, entry, 10.0F);
}

/// Runs frames so a changed cvar or scene has reached the presented image.
inline bool settle_frames(engine::EnginePipeline &pipeline,
                          int frames = 6) noexcept {
  for (int frame = 0; frame < frames; ++frame) {
    if (!pipeline.execute_frame()) {
      return false;
    }
  }
  return true;
}

/// Boots the engine windowed, runs one frame, empties the World, runs
/// body, and tears everything down. Exit codes 1 to 6 are the fixture's
/// own: assets not found, bootstrap, pipeline initialization, no World, a
/// cvar the fixture could not set, a cvar the body could not set. The
/// frame that runs first is what registers the renderer's cvars, so a body
/// can set them from its first line; it does so through checked().
///
/// With firstFrameScene the World is emptied and that scene authored before
/// the first frame instead, and the body receives it as drawn: exit code 7
/// is a first-frame scene that could not be built.
inline int
run_gpu_scene_test(const char *name, GpuSceneBody body,
                   GpuFirstFrameScene firstFrameScene = nullptr) noexcept {
  if (!detail::enter_asset_directory()) {
    return 1;
  }
  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &detail::capture_world;
  bridge.is_playing = &detail::always_playing;
  bridge.is_paused = &detail::never_paused;
  engine::runtime::set_editor_bridge(&bridge);

  if (!engine::bootstrap()) {
    engine::runtime::set_editor_bridge(nullptr);
    return 2;
  }

  int result = 3;
  {
    engine::EnginePipeline pipeline;
    if (pipeline.initialize(0U)) {
      result = 4;
      const engine::renderer::RenderDevice *device =
          engine::renderer::render_device();
      if ((device == nullptr) || !device->caps.cookedPrograms) {
        std::printf("SKIPPED: no render device with cooked programs\n");
        result = 0;
      } else if (detail::g_fixtureWorld != nullptr) {
        // The renderer registers most of its cvars when its backend builds
        // itself, which is on the first flush, and a set on a name that is
        // not registered yet is refused. So one frame runs before anything
        // is configured, and every set is checked: a refused one would
        // leave the test running in a configuration it does not describe.
        //
        // A frame must depend on nothing but the scene and whatever the
        // test varies: auto exposure adapts over frames, so two captures
        // of one scene would differ. Outside player mode the back buffer
        // is left black for the editor's overlay; r_present_scene is what
        // puts the final image where the readback looks.
        bool firstSceneBuilt = true;
        if (firstFrameScene != nullptr) {
          engine::runtime::reset_world(*detail::g_fixtureWorld);
          firstSceneBuilt = firstFrameScene(*detail::g_fixtureWorld);
        }
        //
        // The three that bootstrap registers are set before that frame, as
        // a game's loaded settings would be, so it already runs unthrottled
        // — and, r_vsync having left its boot value, it is also a frame on
        // which the device resets its swapchain.
        const bool configured =
            firstSceneBuilt && core::cvar_set_int("r_vsync", 0) &&
            core::cvar_set_int("r_max_fps", 0) &&
            core::cvar_set_bool("r_present_scene", true) &&
            pipeline.execute_frame() &&
            core::cvar_set_bool("r_auto_exposure", false) &&
            core::cvar_set_bool("r_deferred", true);
        if (!firstSceneBuilt) {
          std::fprintf(stderr, "FAIL: the first-frame scene was refused\n");
          result = 7;
        } else if (!configured) {
          std::fprintf(stderr, "FAIL: the fixture could not configure the "
                               "renderer's cvars\n");
          result = 5;
        } else {
          if (firstFrameScene == nullptr) {
            engine::runtime::reset_world(*detail::g_fixtureWorld);
          }
          detail::g_cvarRefused = false;
          result = body(pipeline, *detail::g_fixtureWorld);
          if ((result == 0) && detail::g_cvarRefused) {
            result = 6;
          }
        }
      }
    }
    pipeline.teardown();
  }
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();
  detail::g_fixtureWorld = nullptr;

  if (result == 0) {
    std::printf("%s: done\n", name);
  }
  return result;
}

} // namespace engine::tests
