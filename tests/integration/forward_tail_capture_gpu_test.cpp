// GPU regression for issues #632 and #640: a pass appearing earlier in a
// frame must not move where the deferred path's forward tail draws.
//
// The deferred path draws the transparent tail, and any opaque run the
// G-buffer cannot express, forward over the deferred depth. Each bind
// claims a fresh bgfx view, a view's rect persists per id across frames,
// and two of those binds set no rect of their own, so the tail drew with
// whatever rect its view id last carried. Any pass that claims a
// different number of views before it therefore moved the tail: a scene
// capture did it (#632, a translucent slab over a screen fraction with
// the water missing from its place) and so did the directional shadow
// cache invalidating on a camera move (#640, the same loss for one frame).
//
// A unit test holds the device call order (engine_unit_deferred_pass_
// viewport). Only an image shows the tail landing where it belongs, and
// only a frame that adds a pass shows it staying there — which is why
// this exists as well and why it adds a scene capture rather than trusting
// the counters.

#include "../gpu_scene_fixture.h"

#include "engine/math/quat.h"

#include <cmath>
#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;
using engine::tests::mean_channel;

/// The translucent panel's own colour: strongly blue, so the tail's
/// contribution is separable from the warm floor and the sky behind it
/// by a channel difference rather than by brightness alone.
constexpr float kPanelRed = 0.02F;
constexpr float kPanelGreen = 0.06F;
constexpr float kPanelBlue = 0.90F;

double blueness(const CapturedFrame &frame, std::uint32_t x0, std::uint32_t y0,
                std::uint32_t x1, std::uint32_t y1) noexcept {
  return mean_channel(frame, 2U, x0, y0, x1, y1) -
         mean_channel(frame, 0U, x0, y0, x1, y1);
}

/// A translucent panel filling the middle of the view, drawn by the
/// deferred path's forward tail. The floor is opaque and goes through the
/// G-buffer, so the frame exercises both halves of the split.
int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::settle_frames;

  // Fog would mix the panel toward the sky and blunt the very channel
  // difference this measures (#641).
  engine::tests::checked(engine::core::cvar_set_string("r_fog_mode", "off"),
                         "r_fog_mode");
  engine::tests::checked(engine::core::cvar_set_bool("r_height_fog", false),
                         "r_height_fog");
  // The defect is the deferred path's; the forward path pairs all of its
  // binds with a viewport and never showed it.
  engine::tests::checked(engine::core::cvar_set_bool("r_deferred", true),
                         "r_deferred");

  engine::runtime::Transform floorTransform{};
  floorTransform.scale = engine::math::Vec3(60.0F, 1.0F, 60.0F);
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.direction = engine::math::Vec3(-0.3F, -0.8F, -0.5F);
  sunLight.intensity = 2.5F;
  if ((engine::tests::add_builtin_mesh(
           world, "builtin://plane", floorTransform,
           engine::math::Vec3(0.9F, 0.75F, 0.5F)) == kInvalidEntity) ||
      (sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 2.0F, 8.0F),
                                engine::math::Vec3(0.0F, 2.0F, 0.0F))) {
    return 10;
  }

  engine::runtime::Transform panelTransform{};
  panelTransform.position = engine::math::Vec3(0.0F, 2.0F, 0.0F);
  panelTransform.scale = engine::math::Vec3(4.0F, 4.0F, 0.2F);
  const Entity panel = world.create_scene_object(panelTransform);
  engine::runtime::MeshComponent panelMesh{};
  panelMesh.meshAssetId =
      engine::content::make_asset_id_from_path("builtin://cube");
  panelMesh.albedo = engine::math::Vec3(kPanelRed, kPanelGreen, kPanelBlue);
  // Below one, so render prep sorts it into the transparent half and the
  // flush draws it in the forward tail rather than through the G-buffer.
  panelMesh.opacity = 0.6F;
  if ((panel == kInvalidEntity) ||
      !world.add_mesh_component(panel, panelMesh)) {
    return 11;
  }

  CapturedFrame plain{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "forward_tail_plain.tga", &plain)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  const std::uint32_t w = plain.width;
  const std::uint32_t h = plain.height;
  // Where the panel is, and a corner it must never reach. #632's slab
  // covered the top-left quarter, so that corner is the witness.
  const std::uint32_t cx0 = (w * 2U) / 5U;
  const std::uint32_t cx1 = (w * 3U) / 5U;
  const std::uint32_t cy0 = (h * 2U) / 5U;
  const std::uint32_t cy1 = (h * 3U) / 5U;
  const std::uint32_t qx1 = w / 4U;
  const std::uint32_t qy1 = h / 4U;

  const double centrePlain = blueness(plain, cx0, cy0, cx1, cy1);
  const double cornerPlain = blueness(plain, 0U, 0U, qx1, qy1);

  // Adding a scene capture claims views before the tail's, which is what
  // moved the tail's rect on base. The capture needs no consumer: the
  // pass runs for the request, and the views it claims are the mechanism.
  engine::runtime::Transform captureTransform{};
  captureTransform.position = engine::math::Vec3(0.0F, 3.0F, 6.0F);
  captureTransform.rotation = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 1.0F, 0.0F), 3.14159265F);
  const Entity capture = world.create_scene_object(captureTransform);
  engine::runtime::SceneCaptureComponent captureComponent{};
  if ((capture == kInvalidEntity) ||
      !world.add_scene_capture_component(capture, captureComponent)) {
    return 12;
  }

  CapturedFrame captured{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "forward_tail_capture.tga",
                               &captured)) {
    return 13;
  }

  const double centreCaptured = blueness(captured, cx0, cy0, cx1, cy1);
  const double cornerCaptured = blueness(captured, 0U, 0U, qx1, qy1);

  std::printf("forward_tail_capture_gpu_test: centre blueness %.1f plain, "
              "%.1f with a capture; corner %.1f then %.1f\n",
              centrePlain, centreCaptured, cornerPlain, cornerCaptured);

  int result = 0;
  // The panel must be there at all, or the rest measures an empty frame.
  if (centrePlain < 20.0) {
    std::fprintf(stderr,
                 "FAIL: the translucent panel is not in the middle of the "
                 "plain frame (blueness %.1f)\n",
                 centrePlain);
    result = 20;
  }
  // The tail must not move when a pass appears before it. Twelve levels
  // is far below losing the panel (which would drop the centre to the
  // floor's negative blueness, tens of levels away) and far above the
  // frame-to-frame noise of a static scene.
  if (std::fabs(centreCaptured - centrePlain) > 12.0) {
    std::fprintf(stderr,
                 "FAIL: adding a scene capture changed the forward tail in "
                 "the middle of the view by %.1f levels\n",
                 centreCaptured - centrePlain);
    result = 21;
  }
  // And it must not appear anywhere else: #632's signature was the tail
  // drawn over a corner it does not cover.
  if ((cornerCaptured - cornerPlain) > 12.0) {
    std::fprintf(stderr,
                 "FAIL: adding a scene capture put the forward tail over a "
                 "corner it does not cover (blueness rose %.1f)\n",
                 cornerCaptured - cornerPlain);
    result = 22;
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("forward_tail_capture_gpu_test",
                                           &run);
}
