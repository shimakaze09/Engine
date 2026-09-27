// Verifies on a real render device that the editor's Scene view, rendered
// in the same frames from another camera and at another size, leaves the
// Game view's presented image exactly as it is without one. The two views
// share the cascade atlas and the frame's device state, and each keeps its
// own targets, tile light table and shadow-cache key; a view drawing into
// the other's state, or reusing cascades the other view left in the atlas,
// changes the Game view's pixels.

#include "../gpu_scene_fixture.h"

#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

const engine::math::Vec3 kSceneEye(-14.0F, 9.0F, -12.0F);

/// A Scene view far round the other side of the scene, at 200x120.
bool scene_view(engine::renderer::RenderViewDesc *outView) noexcept {
  outView->camera.position = kSceneEye;
  outView->camera.target = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  outView->width = 200;
  outView->height = 120;
  return true;
}

/// A floor, a cube casting a cascade shadow across it, and a point light:
/// shadows exercise the shared atlas, the point light the tile table.
bool author_scene(World &world) noexcept {
  engine::runtime::Transform cubeTransform{};
  cubeTransform.position = engine::math::Vec3(-3.0F, 1.0F, 0.0F);
  cubeTransform.scale = engine::math::Vec3(2.0F, 2.0F, 2.0F);
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.direction = engine::math::Vec3(1.0F, -0.6F, 0.0F);
  sunLight.intensity = 3.0F;
  engine::runtime::Transform lampTransform{};
  lampTransform.position = engine::math::Vec3(2.0F, 1.5F, 2.0F);
  const Entity lamp = world.create_scene_object(lampTransform);
  engine::runtime::PointLightComponent lampLight{};
  lampLight.color = engine::math::Vec3(1.0F, 0.4F, 0.2F);
  lampLight.intensity = 4.0F;
  lampLight.radius = 6.0F;
  return (engine::tests::add_builtin_mesh(
              world, "builtin://plane",
              engine::tests::framed_floor_transform(60.0F),
              engine::math::Vec3(0.8F, 0.8F, 0.8F)) != kInvalidEntity) &&
         (engine::tests::add_builtin_mesh(
              world, "builtin://cube", cubeTransform,
              engine::math::Vec3(0.8F, 0.8F, 0.8F)) != kInvalidEntity) &&
         (sun != kInvalidEntity) && world.add_light_component(sun, sunLight) &&
         (lamp != kInvalidEntity) &&
         world.add_point_light_component(lamp, lampLight) &&
         engine::tests::look_from(world, engine::math::Vec3(0.0F, 6.0F, 10.0F),
                                  engine::math::Vec3(0.0F, 0.0F, 0.0F));
}

int run(engine::EnginePipeline &pipeline, World &) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::mean_abs_difference;
  using engine::tests::settle_frames;

  CapturedFrame alone{};
  if (!settle_frames(pipeline, 12) ||
      !capture_presented_frame(pipeline, "render_views_game_alone.tga",
                               &alone)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  CapturedFrame aloneAgain{};
  if (!settle_frames(pipeline) ||
      !capture_presented_frame(pipeline, "render_views_game_alone_again.tga",
                               &aloneAgain)) {
    return 10;
  }

  const std::uint64_t sceneFramesBefore =
      engine::renderer::render_view_frame_count(
          engine::renderer::RenderViewId::Scene);
  engine::tests::detail::g_sceneView = &scene_view;
  CapturedFrame beside{};
  const bool captured =
      settle_frames(pipeline, 4) &&
      capture_presented_frame(pipeline, "render_views_game_beside_scene.tga",
                              &beside);
  const std::uint64_t sceneFrames = engine::renderer::render_view_frame_count(
      engine::renderer::RenderViewId::Scene);
  const engine::renderer::CameraState sceneCamera =
      engine::renderer::render_view_camera(
          engine::renderer::RenderViewId::Scene);
  engine::tests::detail::g_sceneView = nullptr;
  if (!captured) {
    return 11;
  }

  const std::uint32_t w = alone.width;
  const std::uint32_t h = alone.height;
  const double noise = mean_abs_difference(alone, aloneAgain, 0U, 0U, w, h);
  const double drift = mean_abs_difference(alone, beside, 0U, 0U, w, h);
  std::printf("render_views_isolation_gpu_test: frame-to-frame %.4f levels, "
              "alone vs beside a Scene view %.4f levels; Scene view frames "
              "%llu\n",
              noise, drift,
              static_cast<unsigned long long>(sceneFrames - sceneFramesBefore));

  if (sceneFrames <= sceneFramesBefore) {
    std::fprintf(stderr, "FAIL: the Scene view never rendered\n");
    return 12;
  }
  if ((sceneCamera.position.x != kSceneEye.x) ||
      (sceneCamera.position.y != kSceneEye.y) ||
      (sceneCamera.position.z != kSceneEye.z)) {
    std::fprintf(stderr, "FAIL: the Scene view rendered another camera\n");
    return 13;
  }
  // A still scene renders the same pixels frame after frame, so the Game
  // view beside a Scene view must match it exactly: the frame-to-frame
  // difference is the whole allowance.
  if ((alone.width != beside.width) || (alone.height != beside.height) ||
      (drift > noise)) {
    std::fprintf(stderr,
                 "FAIL: rendering the Scene view changed the Game view by "
                 "%.4f levels (frame-to-frame %.4f)\n",
                 drift, noise);
    return 14;
  }
  return 0;
}

} // namespace

int main() {
  return engine::tests::run_gpu_scene_test("render_views_isolation_gpu_test",
                                           &run, &author_scene);
}
