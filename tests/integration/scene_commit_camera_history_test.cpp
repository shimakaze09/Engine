// Regression test for #346: a committed scene replacement must retire the
// pipeline's camera interpolation history and give a camera-less
// replacement scene an explicit default view. Drives the production
// EnginePipeline headlessly (the pipeline_clock_reset_test.cpp bootstrap
// pattern) through script-style scene requests and reads the camera the
// render stage used from the editor bridge's render callback, which the
// pipeline invokes after the flush and before it restores the
// un-interpolated sample — the same point the editor overlay reads it.
//
// Observables: renderer::get_active_camera() between frames (the camera the
// replacement World established), the camera recorded inside the bridge
// render callback (the camera the flush and the audio listener consumed),
// and World::content_epoch() (whether a commit actually happened).

#include "engine/engine.h"
#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <thread>

namespace {

constexpr const char *kNoCameraSceneFile = "scch_no_camera.scene.json";
constexpr const char *kFarCameraSceneFile = "scch_far_camera.scene.json";
constexpr const char *kMissingSceneFile = "scch_missing.scene.json";
constexpr const char *kNewSceneScript = "scch_new_scene.lua";

// The two authored camera positions are far apart so any blend between
// them, at any alpha, lands well away from both.
const engine::math::Vec3 kNearPosition(10.0F, 5.0F, 10.0F);
const engine::math::Vec3 kFarPosition(500.0F, 20.0F, -300.0F);

engine::runtime::World *g_world = nullptr;
engine::renderer::CameraState g_renderedCamera{};
bool g_renderedCameraValid = false;
int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Captures the pipeline's world so the test can author entities and read
/// the content epoch.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool bridge_is_playing() noexcept { return true; }
bool bridge_is_paused() noexcept { return false; }

/// Records the camera the flush consumed: the pipeline calls the bridge's
/// render callback after flush_renderer and before it restores the
/// un-interpolated current sample.
void bridge_render(float, float) noexcept {
  g_renderedCamera = engine::renderer::get_active_camera();
  g_renderedCameraValid = true;
}

bool vec3_equal(const engine::math::Vec3 &a,
                const engine::math::Vec3 &b) noexcept {
  return (a.x == b.x) && (a.y == b.y) && (a.z == b.z);
}

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

bool write_text_file(const char *path, const char *contents) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(contents);
  const bool ok = std::fwrite(contents, 1U, length, file) == length;
  std::fclose(file);
  return ok;
}

void cleanup_files() noexcept {
  static_cast<void>(std::remove(kNoCameraSceneFile));
  static_cast<void>(std::remove(kFarCameraSceneFile));
  static_cast<void>(std::remove(kNewSceneScript));
}

/// Adds a scene object at `position`, optionally carrying an authored
/// CameraComponent, to `world`.
bool author_entity(engine::runtime::World &world,
                   const engine::math::Vec3 &position,
                   bool withCamera) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  if (entity == engine::runtime::kInvalidEntity) {
    return false;
  }
  if (!withCamera) {
    return true;
  }
  engine::runtime::CameraComponent camera{};
  return world.add_camera_component(entity, camera);
}

/// Writes the two fixture scenes through the production scene serializer.
bool write_scene_fixtures() noexcept {
  {
    std::unique_ptr<engine::runtime::World> world(
        new (std::nothrow) engine::runtime::World());
    if ((world == nullptr) ||
        !author_entity(*world, engine::math::Vec3(1.0F, 1.0F, 1.0F), false) ||
        !engine::runtime::save_scene(*world, kNoCameraSceneFile)) {
      return false;
    }
  }
  {
    std::unique_ptr<engine::runtime::World> world(
        new (std::nothrow) engine::runtime::World());
    if ((world == nullptr) || !author_entity(*world, kFarPosition, true) ||
        !engine::runtime::save_scene(*world, kFarCameraSceneFile)) {
      return false;
    }
  }
  return write_text_file(kNewSceneScript, "engine.new_scene()\n");
}

/// Runs one playing frame guaranteed to simulate at least one fixed step
/// (see pipeline_tick_cadence_test.cpp on the wall-clock accumulator).
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return pipeline.execute_frame();
}

/// Runs the frame that commits a pending scene op, then the first frame the
/// replacement World is rendered in, and returns the camera that second
/// frame's flush consumed through `outRendered`.
bool commit_and_render_first_frame(
    engine::EnginePipeline &pipeline,
    engine::renderer::CameraState *outRendered) noexcept {
  if (!ticking_frame(pipeline)) {
    return false;
  }
  g_renderedCameraValid = false;
  if (!ticking_frame(pipeline) || !g_renderedCameraValid) {
    return false;
  }
  *outRendered = g_renderedCamera;
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  cleanup_files();
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_scene_fixtures()) {
    std::fprintf(stderr, "FAIL: write scene fixtures\n");
    cleanup_files();
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  bridge.render = &bridge_render;
  engine::runtime::set_editor_bridge(&bridge);

  // Full production bootstrap in headless mode: the null render device
  // stands in so the pipeline runs on every CI lane.
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    cleanup_files();
    return 2;
  }

  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U) || (g_world == nullptr)) {
    std::fprintf(stderr, "FAIL: pipeline initialize\n");
    pipeline.teardown();
    engine::shutdown();
    cleanup_files();
    return 3;
  }

  const engine::renderer::CameraState defaultCamera{};

  // Guard: the live scene's authored camera is what the renderer presents
  // before any transition, otherwise the transition cases prove nothing.
  CHECK(author_entity(*g_world, kNearPosition, true), "author near camera");
  CHECK(ticking_frame(pipeline), "frame: near camera published");
  CHECK(vec3_equal(engine::renderer::get_active_camera().position,
                   kNearPosition),
        "guard: the live scene's camera is active before any transition");

  // --- Case 1: a camera-less replacement presents the default camera. ---
  // Base behavior: nothing republishes a camera for the new World, so the
  // discarded scene's view (and the listener that follows it) stays
  // active indefinitely.
  {
    const std::uint32_t epochBefore = g_world->content_epoch();
    CHECK(engine::scripting::request_scene_load(kNoCameraSceneFile),
          "request camera-less scene");
    engine::renderer::CameraState rendered{};
    CHECK(commit_and_render_first_frame(pipeline, &rendered),
          "camera-less: commit + first frame");
    CHECK(g_world->content_epoch() != epochBefore,
          "camera-less: the load committed");
    CHECK(g_world->camera_manager().camera_count() == 0U,
          "camera-less: the replacement publishes no camera");
    CHECK(vec3_equal(engine::renderer::get_active_camera().position,
                     defaultCamera.position),
          "camera-less replacement presents the default camera, not the "
          "discarded scene's view");
    CHECK(vec3_equal(rendered.position, defaultCamera.position),
          "camera-less: the first rendered frame used the default camera");
  }

  // --- Case 2: the replacement's first frame is not blended from the
  // discarded World's camera. ---
  // Base behavior: the samples taken from the previous World stay valid,
  // so at render alpha < 1 the flush (and the listener) consume a lerp
  // between the old view and the new scene's camera.
  {
    const std::uint32_t epochBefore = g_world->content_epoch();
    CHECK(engine::scripting::request_scene_load(kFarCameraSceneFile),
          "request far-camera scene");
    engine::renderer::CameraState rendered{};
    CHECK(commit_and_render_first_frame(pipeline, &rendered),
          "far camera: commit + first frame");
    CHECK(g_world->content_epoch() != epochBefore,
          "far camera: the load committed");
    const engine::renderer::CameraState established =
        engine::renderer::get_active_camera();
    CHECK(vec3_equal(established.position, kFarPosition),
          "far camera: the replacement's authored camera is established");
    CHECK(vec3_equal(rendered.position, established.position),
          "the first frame after a commit renders the replacement's camera, "
          "not a blend with the discarded World's");
  }

  // --- Case 3: a failed load changes nothing. ---
  // The missing file fails inside load_scene, so the World, its epoch and
  // the live camera history all survive; the request stays queued (the
  // pipeline's retry policy for a failed op) and is dropped here so it
  // cannot leak into the later cases.
  {
    const std::uint32_t epochBefore = g_world->content_epoch();
    CHECK(engine::scripting::request_scene_load(kMissingSceneFile),
          "request missing scene");
    engine::renderer::CameraState rendered{};
    CHECK(commit_and_render_first_frame(pipeline, &rendered),
          "failed load: frames run");
    CHECK(g_world->content_epoch() == epochBefore,
          "failed load: no commit happened");
    CHECK(vec3_equal(engine::renderer::get_active_camera().position,
                     kFarPosition),
          "failed load: the live camera survives");
    CHECK(vec3_equal(rendered.position, kFarPosition),
          "failed load: rendering continues from the live camera");
    engine::scripting::clear_pending_scene_op();
  }

  // --- Case 4: engine.new_scene empties the World; the default camera
  // replaces the discarded scene's view. ---
  {
    const std::uint32_t epochBefore = g_world->content_epoch();
    CHECK(engine::scripting::load_script(kNewSceneScript),
          "request new scene from Lua");
    CHECK(engine::scripting::has_pending_scene_op(), "new scene queued");
    engine::renderer::CameraState rendered{};
    CHECK(commit_and_render_first_frame(pipeline, &rendered),
          "new scene: commit + first frame");
    CHECK(g_world->content_epoch() != epochBefore,
          "new scene: the reset committed");
    CHECK(vec3_equal(engine::renderer::get_active_camera().position,
                     defaultCamera.position),
          "new scene presents the default camera");
    CHECK(vec3_equal(rendered.position, defaultCamera.position),
          "new scene: the first rendered frame used the default camera");
  }

  // --- Case 5: repeated transitions keep the contract. ---
  {
    CHECK(engine::scripting::request_scene_load(kFarCameraSceneFile),
          "request far-camera scene again");
    engine::renderer::CameraState rendered{};
    CHECK(commit_and_render_first_frame(pipeline, &rendered),
          "repeat: commit + first frame");
    CHECK(vec3_equal(rendered.position, kFarPosition),
          "repeat: the first frame renders the replacement's camera");
    // A frame with no transition interpolates between two samples of the
    // same World, which for a static camera is that camera exactly.
    g_renderedCameraValid = false;
    CHECK(ticking_frame(pipeline), "repeat: steady frame");
    CHECK(g_renderedCameraValid &&
              vec3_equal(g_renderedCamera.position, kFarPosition),
          "steady state: the live camera keeps rendering");
  }

  pipeline.teardown();
  engine::shutdown();
  cleanup_files();

  if (g_failures != 0) {
    std::fprintf(stderr, "scene_commit_camera_history_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("scene_commit_camera_history_test: all checks passed\n");
  return 0;
}
