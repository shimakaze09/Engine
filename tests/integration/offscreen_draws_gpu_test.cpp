// GPU regression for issue #524: what the main camera cannot see still
// reaches the passes that need it. Render prep culls the draw list against
// the camera; on base the shadow and capture passes walked that same list,
// so a caster just outside the view cast nothing into it and a scene
// capture facing away from the player held only its clear colour. The
// pipeline suite counts the draws kept for those passes; only the image
// shows they are then drawn where they should be.
//
// Both subjects sit behind the camera. A slab there, with the sun behind
// the camera too, must put its shadow on the floor in front. A capture
// camera there, turned to face a red cube the player cannot see, must show
// that cube on a panel in front of the player. The built-in meshes carry
// no texture coordinates, so the panel shows a single texel of the
// capture; the cube therefore fills the capture's whole view, and any
// texel of it is red only if the capture drew what it faces.

#include "../gpu_scene_fixture.h"

#include "engine/math/quat.h"

#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

double mean_brightness(const CapturedFrame &frame, std::uint32_t x0,
                       std::uint32_t y0, std::uint32_t x1,
                       std::uint32_t y1) noexcept {
  return (engine::tests::mean_channel(frame, 0U, x0, y0, x1, y1) +
          engine::tests::mean_channel(frame, 1U, x0, y0, x1, y1) +
          engine::tests::mean_channel(frame, 2U, x0, y0, x1, y1)) /
         3.0;
}

/// The player looks from (0, 6, 10) at the origin, so everything with z
/// beyond 10 and above the floor is behind the view.
int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::settle_frames;

  engine::tests::checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  const engine::runtime::Transform floorTransform =
      engine::tests::framed_floor_transform(60.0F);
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  // From behind the camera, forward and down: a caster behind the view
  // throws its shadow ahead into it.
  sunLight.direction = engine::math::Vec3(0.0F, -0.4F, -1.0F);
  sunLight.intensity = 3.0F;
  if ((engine::tests::add_builtin_mesh(world, "builtin://plane", floorTransform,
                                       engine::math::Vec3(1.0F, 1.0F, 1.0F)) ==
       kInvalidEntity) ||
      (sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 6.0F, 10.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  CapturedFrame open{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "offscreen_open.tga", &open)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  // Four metres up and four behind the camera. Along the sun's direction
  // it meets the floor ten metres on, at z = 4: in front of the player,
  // a little below the middle of the view.
  engine::runtime::Transform slabTransform{};
  slabTransform.position = engine::math::Vec3(0.0F, 4.0F, 14.0F);
  slabTransform.scale = engine::math::Vec3(5.0F, 0.4F, 5.0F);
  const Entity slab =
      engine::tests::add_builtin_mesh(world, "builtin://cube", slabTransform,
                                      engine::math::Vec3(0.8F, 0.8F, 0.8F));
  CapturedFrame shaded{};
  if ((slab == kInvalidEntity) || !settle_frames(pipeline, 10) ||
      !capture_presented_frame(pipeline, "offscreen_shaded.tga", &shaded)) {
    return 11;
  }
  const std::uint32_t w = open.width;
  const std::uint32_t h = open.height;
  const double floorOpen =
      mean_brightness(open, (w * 2U) / 5U, h / 2U, (w * 3U) / 5U, (h * 4U) / 5U);
  const double floorShaded = mean_brightness(shaded, (w * 2U) / 5U, h / 2U,
                                             (w * 3U) / 5U, (h * 4U) / 5U);
  std::printf("offscreen_draws_gpu_test: floor ahead %.1f open, %.1f with a "
              "caster behind the camera (%.1f darker)\n",
              floorOpen, floorShaded, floorOpen - floorShaded);

  // A panel ahead of the player shows what a capture camera sees. The
  // capture sits behind the player and is turned half a turn about the
  // vertical, so it faces away from everything the player sees, at a red
  // cube further back still. The sun lights the panel, which faces the
  // player, and so cannot also light the face the capture looks at; a
  // point light beside the capture does that. Unlit, red reads as grey
  // under the sky.
  engine::runtime::Transform lampTransform{};
  lampTransform.position = engine::math::Vec3(0.0F, 2.0F, 20.2F);
  const Entity lamp = world.create_scene_object(lampTransform);
  engine::runtime::PointLightComponent lampLight{};
  lampLight.intensity = 6.0F;
  lampLight.radius = 12.0F;
  if (!world.destroy_entity(slab) || (lamp == kInvalidEntity) ||
      !world.add_point_light_component(lamp, lampLight)) {
    return 12;
  }
  engine::runtime::Transform captureTransform{};
  captureTransform.position = engine::math::Vec3(0.0F, 2.0F, 20.0F);
  captureTransform.rotation = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 1.0F, 0.0F), 3.14159265F);
  const Entity capture = world.create_scene_object(captureTransform);
  engine::runtime::SceneCaptureComponent captureComponent{};
  engine::runtime::Transform subjectTransform{};
  // Ten metres across with its near face one metre from the capture: at a
  // 60 degree field of view that face covers the whole capture.
  subjectTransform.position = engine::math::Vec3(0.0F, 2.0F, 26.0F);
  subjectTransform.scale = engine::math::Vec3(10.0F, 10.0F, 10.0F);
  engine::runtime::Transform panelTransform{};
  panelTransform.position = engine::math::Vec3(0.0F, 2.5F, 0.0F);
  panelTransform.scale = engine::math::Vec3(4.0F, 4.0F, 0.2F);
  const Entity panel = world.create_scene_object(panelTransform);
  engine::runtime::MeshComponent panelMesh{};
  panelMesh.meshAssetId =
      engine::content::make_asset_id_from_path("builtin://cube");
  if ((capture == kInvalidEntity) || (panel == kInvalidEntity) ||
      !world.add_scene_capture_component(capture, captureComponent) ||
      (engine::tests::add_builtin_mesh(world, "builtin://cube",
                                       subjectTransform,
                                       engine::math::Vec3(1.0F, 0.02F, 0.02F)) ==
       kInvalidEntity)) {
    return 13;
  }
  panelMesh.sceneCaptureSourceId = world.persistent_id(capture);
  if (!world.add_mesh_component(panel, panelMesh)) {
    return 14;
  }
  CapturedFrame withCapture{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "offscreen_capture.tga",
                                &withCapture)) {
    return 15;
  }
  // The panel covers the middle of the view.
  const std::uint32_t px0 = (w * 9U) / 20U;
  const std::uint32_t px1 = (w * 11U) / 20U;
  const std::uint32_t py0 = (h * 7U) / 20U;
  const std::uint32_t py1 = (h * 10U) / 20U;
  const double panelRed =
      engine::tests::mean_channel(withCapture, 2U, px0, py0, px1, py1);
  const double panelGreen =
      engine::tests::mean_channel(withCapture, 1U, px0, py0, px1, py1);
  std::printf("offscreen_draws_gpu_test: panel centre R/G %.1f/%.1f showing "
              "a capture that faces away from the player\n",
              panelRed, panelGreen);

  int result = 0;
  // With the caster culled away the two frames are the same frame and the
  // difference is the frame-to-frame zero; a sunlit white floor going
  // into shadow loses tens of levels. Five is far from both.
  if ((floorOpen - floorShaded) < 5.0) {
    std::fprintf(stderr, "FAIL: a caster behind the camera darkened the "
                         "floor ahead by only %.1f levels\n",
                 floorOpen - floorShaded);
    result = 20;
  }
  // A capture that drew only what the player's camera kept holds sky,
  // which leans blue-green (red about twenty levels under green); the lit
  // cube puts red far above green.
  if ((panelRed - panelGreen) < 20.0) {
    std::fprintf(stderr, "FAIL: the panel does not show the cube the "
                         "capture faces (R-G %.1f)\n",
                 panelRed - panelGreen);
    result = 21;
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("offscreen_draws_gpu_test", &run);
}
