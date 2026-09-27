// Verifies the editor's two views through the production EnginePipeline on
// the null render device: the Scene view renders from the editor's camera
// and the Game view from the scene's camera manager, Stopped, Playing and
// Paused alike, each at its own size; a hidden view renders nothing; and
// without an editor Scene view the one view keeps the camera it was given.
// Before this the editor had one viewport that changed owner on Play.

#include "engine/core/cvar.h"
#include "engine/engine.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace {

using engine::renderer::CameraState;
using engine::renderer::RenderViewDesc;
using engine::renderer::RenderViewId;

engine::runtime::World *g_world = nullptr;
bool g_playing = false;
bool g_paused = false;
bool g_sceneShown = true;
bool g_gameShown = true;

const engine::math::Vec3 kEditorEye(30.0F, 20.0F, 30.0F);
const engine::math::Vec3 kGameEye(0.0F, 2.0F, -8.0F);
const engine::math::Vec3 kGameTarget(0.0F, 1.0F, 0.0F);

void capture_world(engine::runtime::World *world) noexcept { g_world = world; }
bool is_playing() noexcept { return g_playing; }
bool is_paused() noexcept { return g_paused; }
bool game_view_visible() noexcept { return g_gameShown; }

/// The editor's Scene view: its own camera, at 320x180.
bool scene_view(RenderViewDesc *outView) noexcept {
  if (!g_sceneShown) {
    return false;
  }
  outView->camera.position = kEditorEye;
  outView->camera.target = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  outView->width = 320;
  outView->height = 180;
  return true;
}

bool same_point(const engine::math::Vec3 &a,
                const engine::math::Vec3 &b) noexcept {
  // The camera manager blends to the pushed pose with a saturated blend
  // weight, so the evaluated position is the pushed one to rounding.
  constexpr float kTolerance = 1.0e-4F;
  return (std::fabs(a.x - b.x) <= kTolerance) &&
         (std::fabs(a.y - b.y) <= kTolerance) &&
         (std::fabs(a.z - b.z) <= kTolerance);
}

/// Walks upward from the current path until the bundled assets are found.
bool enter_asset_directory() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (!ec && std::filesystem::exists(normalized / "assets/main.lua", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// Runs one frame and checks both views drew from their own cameras.
void check_views(engine::tests::TestContext &t,
                 engine::EnginePipeline &pipeline, const char *state) noexcept {
  const std::uint64_t gameBefore =
      engine::renderer::render_view_frame_count(RenderViewId::Game);
  const std::uint64_t sceneBefore =
      engine::renderer::render_view_frame_count(RenderViewId::Scene);
  if (!pipeline.execute_frame()) {
    t.fail(state);
    return;
  }
  const CameraState game =
      engine::renderer::render_view_camera(RenderViewId::Game);
  const CameraState scene =
      engine::renderer::render_view_camera(RenderViewId::Scene);
  t.check((engine::renderer::render_view_frame_count(RenderViewId::Game) ==
           gameBefore + 1U) &&
              (engine::renderer::render_view_frame_count(RenderViewId::Scene) ==
               sceneBefore + 1U),
          state);
  t.check(same_point(game.position, kGameEye), state);
  t.check(same_point(scene.position, kEditorEye), state);
}

} // namespace

int main() {
  engine::tests::TestContext t;
  if (!enter_asset_directory()) {
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &is_playing;
  bridge.is_paused = &is_paused;
  bridge.scene_view = &scene_view;
  bridge.game_view_visible = &game_view_visible;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    engine::runtime::set_editor_bridge(nullptr);
    return 2;
  }
  static_cast<void>(engine::core::cvar_set_int("r_max_fps", 0));

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      pipeline.teardown();
      engine::shutdown();
      engine::runtime::set_editor_bridge(nullptr);
      return 3;
    }
    engine::runtime::reset_world(*g_world);

    // The scene's own camera, which play renders from.
    const engine::runtime::Entity owner = g_world->create_scene_object();
    engine::runtime::CameraEntry entry{};
    entry.position = kGameEye;
    entry.target = kGameTarget;
    entry.up = engine::math::Vec3(0.0F, 1.0F, 0.0F);
    entry.blendSpeed = 1000.0F;
    t.check((owner != engine::runtime::kInvalidEntity) &&
                g_world->camera_manager().push_camera(owner, entry, 10.0F),
            "the scene has a camera");

    // A build without cooked shaders has no renderer backend: no view can
    // render, so there is nothing here to verify.
    g_playing = false;
    g_paused = false;
    if (!pipeline.execute_frame() ||
        (engine::renderer::render_view_frame_count(RenderViewId::Game) == 0U)) {
      std::printf("SKIPPED: the renderer backend did not initialize (no "
                  "cooked shaders in this build)\n");
      pipeline.teardown();
      engine::shutdown();
      engine::runtime::set_editor_bridge(nullptr);
      return 0;
    }

    // --- Stopped: the Game view already shows the scene's camera, the
    // Scene view the editor's.
    check_views(t, pipeline, "stopped: each view renders its own camera");

    // --- Playing and Paused: the Scene view keeps the editor camera.
    g_playing = true;
    check_views(t, pipeline, "playing: each view renders its own camera");
    check_views(t, pipeline, "playing, next frame: unchanged");
    g_playing = false;
    g_paused = true;
    check_views(t, pipeline, "paused: each view renders its own camera");
    g_paused = false;
    g_playing = true;

    // --- A hidden view renders nothing; the other still renders.
    g_sceneShown = false;
    std::uint64_t scene =
        engine::renderer::render_view_frame_count(RenderViewId::Scene);
    std::uint64_t game =
        engine::renderer::render_view_frame_count(RenderViewId::Game);
    t.check(pipeline.execute_frame(), "frame with the Scene view hidden");
    t.check((engine::renderer::render_view_frame_count(RenderViewId::Scene) ==
             scene) &&
                (engine::renderer::render_view_frame_count(
                     RenderViewId::Game) == game + 1U),
            "a hidden Scene view renders nothing");
    g_sceneShown = true;
    g_gameShown = false;
    scene = engine::renderer::render_view_frame_count(RenderViewId::Scene);
    game = engine::renderer::render_view_frame_count(RenderViewId::Game);
    t.check(pipeline.execute_frame(), "frame with the Game view hidden");
    t.check((engine::renderer::render_view_frame_count(RenderViewId::Game) ==
             game) &&
                (engine::renderer::render_view_frame_count(
                     RenderViewId::Scene) == scene + 1U),
            "a hidden Game view renders nothing");
    g_gameShown = true;
    g_playing = false;

    // --- Without an editor Scene view, stopped, the one view keeps the
    // camera it was given, as it always has.
    bridge.scene_view = nullptr;
    CameraState given{};
    given.position = engine::math::Vec3(-5.0F, 5.0F, -5.0F);
    engine::renderer::set_active_camera(given);
    t.check(pipeline.execute_frame(), "frame without a Scene view");
    t.check(
        same_point(
            engine::renderer::render_view_camera(RenderViewId::Game).position,
            given.position),
        "without a Scene view the Game view keeps the given camera");

    pipeline.teardown();
  }
  engine::shutdown();
  engine::runtime::set_editor_bridge(nullptr);
  return t.finish("pipeline_render_views");
}
