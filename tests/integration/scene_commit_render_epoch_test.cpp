// Regression test for #450: one render submission draws one World. A
// pending script scene operation (engine.load_scene / engine.new_scene)
// must not commit between the stages that build a frame's camera and
// prepared draws and the render stage that collects the frame's lights and
// capture requests and flushes it, or the flushed frame mixes the outgoing
// World's geometry and camera with the replacement World's lighting.
//
// Drives the production EnginePipeline headlessly (the
// pipeline_clock_reset_test.cpp bootstrap pattern) and observes every frame
// from the editor bridge's render callback, which the pipeline invokes
// inside the render stage right after the flush: the World the render stage
// read its flush-time inputs from (identified by World::content_epoch()) and
// the camera the flush consumed (renderer::get_active_camera(), published
// by the camera stage before render prep). The contract holds when the two
// always belong to the same World: the near camera renders with the live
// scene's epoch, the far camera with the far scene's epoch, and never
// crosswise. Before the fix the commit ran ahead of the render stage, so
// the commit frame rendered the outgoing camera under the replacement's
// epoch.

#include "engine/engine.h"
#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <thread>

namespace {

constexpr const char *kFarCameraSceneFile = "scre_far_camera.scene.json";
constexpr const char *kMissingSceneFile = "scre_missing.scene.json";
constexpr const char *kNewSceneScript = "scre_new_scene.lua";

// The two authored camera positions are far apart so any blend between
// them, at any alpha, lands well away from both.
const engine::math::Vec3 kNearPosition(10.0F, 5.0F, 10.0F);
const engine::math::Vec3 kFarPosition(500.0F, 20.0F, -300.0F);

engine::runtime::World *g_world = nullptr;

/// What the render stage consumed for the last flushed frame.
struct RenderedFrame final {
  bool valid = false;
  std::uint32_t epoch = 0U;
  engine::math::Vec3 cameraPosition{};
};
RenderedFrame g_rendered{};

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Captures the pipeline's world so the test can author entities and read
/// the content epoch the render stage is flushing under.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool bridge_is_playing() noexcept { return true; }
bool bridge_is_paused() noexcept { return false; }

/// Records the World epoch and camera the flush consumed: the pipeline
/// calls the bridge's render callback after flush_renderer, in the same
/// stage that collected the frame's lights and capture requests from the
/// World, and before it restores the un-interpolated camera sample.
void bridge_render(float, float) noexcept {
  g_rendered.valid = (g_world != nullptr);
  if (g_world != nullptr) {
    g_rendered.epoch = g_world->content_epoch();
  }
  g_rendered.cameraPosition = engine::renderer::get_active_camera().position;
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
  static_cast<void>(std::remove(kFarCameraSceneFile));
  static_cast<void>(std::remove(kNewSceneScript));
}

/// Adds a scene object at `position` carrying an authored CameraComponent.
bool author_camera_entity(engine::runtime::World &world,
                          const engine::math::Vec3 &position) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  if (entity == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::CameraComponent camera{};
  return world.add_camera_component(entity, camera);
}

/// Writes the far-camera fixture scene through the production serializer
/// and the new-scene script.
bool write_fixtures() noexcept {
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if ((world == nullptr) || !author_camera_entity(*world, kFarPosition) ||
      !engine::runtime::save_scene(*world, kFarCameraSceneFile)) {
    return false;
  }
  return write_text_file(kNewSceneScript, "engine.new_scene()\n");
}

/// Runs one playing frame guaranteed to simulate at least one fixed step
/// (see pipeline_tick_cadence_test.cpp on the wall-clock accumulator) and
/// returns what its render stage consumed.
bool ticking_frame(engine::EnginePipeline &pipeline,
                   RenderedFrame *outRendered) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  g_rendered = RenderedFrame{};
  if (!pipeline.execute_frame() || !g_rendered.valid) {
    return false;
  }
  *outRendered = g_rendered;
  return true;
}

/// The frame contract: the camera a frame rendered belongs to the World
/// whose epoch its render stage was flushing under.
bool frame_is_single_epoch(const RenderedFrame &frame, std::uint32_t epoch,
                           const engine::math::Vec3 &camera) noexcept {
  return (frame.epoch == epoch) && vec3_equal(frame.cameraPosition, camera);
}

} // namespace

/// Runs this executable or test program.
int main() {
  cleanup_files();
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_fixtures()) {
    std::fprintf(stderr, "FAIL: write fixtures\n");
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
  RenderedFrame frame{};

  // Guard: the live scene's authored camera renders under the live epoch
  // before any transition, otherwise the transition cases prove nothing.
  CHECK(author_camera_entity(*g_world, kNearPosition), "author near camera");
  const std::uint32_t liveEpoch = g_world->content_epoch();
  CHECK(ticking_frame(pipeline, &frame), "frame: near camera published");
  CHECK(frame_is_single_epoch(frame, liveEpoch, kNearPosition),
        "guard: the live scene renders its own camera under its own epoch");

  // --- Case 1: load_scene. The commit frame still renders the outgoing
  // World whole; the replacement renders whole from its first frame. ---
  // Base behavior: the commit ran ahead of the render stage, so the commit
  // frame flushed the outgoing camera and prepared draws under the
  // replacement's epoch, with the replacement's lights and captures.
  {
    CHECK(engine::scripting::request_scene_load(kFarCameraSceneFile),
          "request far-camera scene");
    CHECK(ticking_frame(pipeline, &frame), "load: commit frame runs");
    CHECK(frame_is_single_epoch(frame, liveEpoch, kNearPosition),
          "load: the commit frame renders the outgoing World whole (its "
          "camera under its epoch)");
    CHECK(g_world->content_epoch() != liveEpoch,
          "load: the commit happened within the frame");
    const std::uint32_t farEpoch = g_world->content_epoch();
    CHECK(ticking_frame(pipeline, &frame), "load: first replacement frame");
    CHECK(frame_is_single_epoch(frame, farEpoch, kFarPosition),
          "load: the replacement's first frame renders its own camera under "
          "its own epoch");
    CHECK(ticking_frame(pipeline, &frame), "load: steady frame");
    CHECK(frame_is_single_epoch(frame, farEpoch, kFarPosition),
          "load: steady state stays single-epoch");
  }

  // --- Case 2: a failed load commits nothing, so every frame keeps
  // rendering the live World under its unchanged epoch. ---
  {
    const std::uint32_t epochBefore = g_world->content_epoch();
    CHECK(engine::scripting::request_scene_load(kMissingSceneFile),
          "request missing scene");
    CHECK(ticking_frame(pipeline, &frame), "failed load: frame runs");
    CHECK(frame_is_single_epoch(frame, epochBefore, kFarPosition),
          "failed load: the frame renders the live World");
    CHECK(g_world->content_epoch() == epochBefore,
          "failed load: no commit happened");
    CHECK(!engine::scripting::has_pending_scene_op(),
          "failed load: the request was consumed by its one attempt");
    CHECK(ticking_frame(pipeline, &frame), "failed load: next frame runs");
    CHECK(frame_is_single_epoch(frame, epochBefore, kFarPosition),
          "failed load: the live World keeps rendering");
  }

  // --- Case 3: engine.new_scene. The commit frame renders the outgoing
  // World whole; the emptied World renders the default camera under its
  // new epoch from its first frame. ---
  {
    const std::uint32_t epochBefore = g_world->content_epoch();
    CHECK(engine::scripting::load_script(kNewSceneScript),
          "request new scene from Lua");
    CHECK(engine::scripting::has_pending_scene_op(), "new scene queued");
    CHECK(ticking_frame(pipeline, &frame), "new scene: commit frame runs");
    CHECK(frame_is_single_epoch(frame, epochBefore, kFarPosition),
          "new scene: the commit frame renders the outgoing World whole");
    CHECK(g_world->content_epoch() != epochBefore,
          "new scene: the reset committed within the frame");
    const std::uint32_t emptyEpoch = g_world->content_epoch();
    CHECK(ticking_frame(pipeline, &frame), "new scene: first frame");
    CHECK(frame_is_single_epoch(frame, emptyEpoch, defaultCamera.position),
          "new scene: the emptied World renders the default camera under "
          "its own epoch");
  }

  // --- Case 4: repeated transitions keep the contract. ---
  {
    const std::uint32_t epochBefore = g_world->content_epoch();
    CHECK(engine::scripting::request_scene_load(kFarCameraSceneFile),
          "request far-camera scene again");
    CHECK(ticking_frame(pipeline, &frame), "repeat: commit frame runs");
    CHECK(frame_is_single_epoch(frame, epochBefore, defaultCamera.position),
          "repeat: the commit frame renders the outgoing World whole");
    const std::uint32_t farEpoch = g_world->content_epoch();
    CHECK(farEpoch != epochBefore, "repeat: the load committed");
    CHECK(ticking_frame(pipeline, &frame), "repeat: first replacement frame");
    CHECK(frame_is_single_epoch(frame, farEpoch, kFarPosition),
          "repeat: the replacement renders whole from its first frame");
  }

  pipeline.teardown();
  engine::shutdown();
  cleanup_files();

  if (g_failures != 0) {
    std::fprintf(stderr, "scene_commit_render_epoch_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("scene_commit_render_epoch_test: all checks passed\n");
  return 0;
}
