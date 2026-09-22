// GPU regression for issues #632 and #640: a pass appearing earlier in a
// frame must not change where the deferred path's forward tail draws.
//
// The deferred path draws the transparent tail, and any opaque run the
// G-buffer cannot express, forward over the deferred depth. Each bind
// claims a fresh bgfx view, a view's rect persists per id across frames,
// and two of those binds set no rect of their own, so the tail drew with
// whatever rect its view id last carried. Any pass that claims a
// different number of views before it therefore moved the tail: a scene
// capture did it (#632) and so did the directional shadow cache
// invalidating on a camera move (#640).
//
// The invariant is that adding a scene capture changes nothing in the
// main view, so the test measures the whole frame against itself rather
// than sampling a colour in a region. Adding the capture on base moved
// the panel across the frame — 42.5% of pixels differed when the owner
// reproduced #632 — and a mean absolute difference over every pixel
// catches that wherever the tail lands, without depending on the panel
// being distinguishable from the sky by any one channel.
//
// A first version of this test sampled a "blueness" in two regions, had
// the channel order backwards, and watched a corner where the sky is
// bluer than the panel so the check could not have fired either way. It
// failed on the fixed build and proved nothing (#646). The positive
// control below exists so that a test which measures nothing cannot pass
// again: the panel must demonstrably enter the frame before its
// stability means anything.

#include "../gpu_scene_fixture.h"

#include "engine/math/quat.h"

#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;
using engine::tests::mean_abs_difference;

/// Compares two frames over every pixel.
double whole_frame_difference(const CapturedFrame &a,
                              const CapturedFrame &b) noexcept {
  return mean_abs_difference(a, b, 0U, 0U, a.width, a.height);
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::settle_frames;

  // Fog is a whole-frame mix that would damp the very differences this
  // measures, and its default reaches standing height (#641).
  engine::tests::checked(engine::core::cvar_set_string("r_fog_mode", "off"),
                         "r_fog_mode");
  engine::tests::checked(engine::core::cvar_set_bool("r_height_fog", false),
                         "r_height_fog");
  // The defect is the deferred path's; the forward path pairs all of its
  // binds with a viewport and never showed it.
  engine::tests::checked(engine::core::cvar_set_bool("r_deferred", true),
                         "r_deferred");

  const engine::runtime::Transform floorTransform =
      engine::tests::framed_floor_transform(60.0F);
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

  // Before the panel exists, so the panel's arrival is measurable.
  CapturedFrame empty{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "forward_tail_empty.tga", &empty)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  // Repeating the capture with nothing changed gives the noise this
  // scene actually has, rather than a threshold guessed in advance.
  CapturedFrame emptyAgain{};
  if (!settle_frames(pipeline, 10) ||
      !capture_presented_frame(pipeline, "forward_tail_empty2.tga",
                               &emptyAgain)) {
    return 11;
  }
  const double noise = whole_frame_difference(empty, emptyAgain);

  engine::runtime::Transform panelTransform{};
  panelTransform.position = engine::math::Vec3(0.0F, 2.0F, 0.0F);
  panelTransform.scale = engine::math::Vec3(4.0F, 4.0F, 0.2F);
  const Entity panel = world.create_scene_object(panelTransform);
  engine::runtime::MeshComponent panelMesh{};
  panelMesh.meshAssetId =
      engine::content::make_asset_id_from_path("builtin://cube");
  panelMesh.albedo = engine::math::Vec3(0.02F, 0.06F, 0.90F);
  // Below one, so render prep sorts it into the transparent half and the
  // flush draws it in the forward tail rather than through the G-buffer.
  panelMesh.opacity = 0.6F;
  if ((panel == kInvalidEntity) ||
      !world.add_mesh_component(panel, panelMesh)) {
    return 12;
  }

  CapturedFrame plain{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "forward_tail_plain.tga", &plain)) {
    return 13;
  }
  const double panelArrival = whole_frame_difference(empty, plain);

  // Adding a scene capture claims views before the tail's, which is what
  // moved the tail's rect on base. The capture needs no consumer: the
  // pass runs for the request, and the views it claims are the
  // mechanism.
  engine::runtime::Transform captureTransform{};
  captureTransform.position = engine::math::Vec3(0.0F, 3.0F, 6.0F);
  captureTransform.rotation = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 1.0F, 0.0F), 3.14159265F);
  const Entity capture = world.create_scene_object(captureTransform);
  engine::runtime::SceneCaptureComponent captureComponent{};
  if ((capture == kInvalidEntity) ||
      !world.add_scene_capture_component(capture, captureComponent)) {
    return 14;
  }

  CapturedFrame captured{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "forward_tail_capture.tga",
                               &captured)) {
    return 15;
  }
  const double captureShift = whole_frame_difference(plain, captured);

  std::printf("forward_tail_capture_gpu_test: noise %.3f, the panel's "
              "arrival %.3f, adding a capture %.3f (levels per channel)\n",
              noise, panelArrival, captureShift);

  int result = 0;
  // The positive control. A frame the panel never entered makes every
  // later comparison meaningless, and a test that measures nothing must
  // fail rather than pass.
  //
  // It is not only a fixture check. The panel's arrival is measured on
  // the build under test, so a build that already draws the tail in the
  // wrong place reports a smaller arrival than a correct one: on the
  // revert that established this test it fell from 8.3 to 2.0. A failure
  // here can therefore mean the defect rather than a broken scene, which
  // is why the message says so instead of only naming the scene.
  if (panelArrival < 2.0) {
    std::fprintf(stderr,
                 "FAIL: adding the translucent panel changed the frame by "
                 "only %.3f levels. Either this scene cannot show where the "
                 "forward tail draws, or the tail is already drawing "
                 "somewhere this frame barely sees — check where the panel "
                 "landed before treating the fixture as the fault\n",
                 panelArrival);
    result = 20;
  }
  // The panel must be the largest thing that happened: if its arrival is
  // not well clear of the noise the thresholds below mean nothing.
  if (panelArrival < (noise * 4.0 + 1.0)) {
    std::fprintf(stderr,
                 "FAIL: the panel's arrival (%.3f) is not clear of this "
                 "scene's own noise (%.3f)\n",
                 panelArrival, noise);
    result = 21;
  }
  // The invariant. Adding a capture must change the main view by noise
  // and nothing more; moving the tail changes a large share of the
  // frame, which on base was 42.5% of pixels.
  if (captureShift > (noise + (panelArrival * 0.25))) {
    std::fprintf(stderr,
                 "FAIL: adding a scene capture changed the frame by %.3f "
                 "levels, against noise %.3f and the panel's own arrival "
                 "%.3f: the forward tail moved\n",
                 captureShift, noise, panelArrival);
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
