// GPU regression for issue #579: a frame on which the device resets its
// swapchain still renders its passes into their own targets. bgfx's reset
// points every view back at the back buffer, and the device issued it
// after the frame's passes had claimed their views, so on that frame the
// shadow cascades, the BRDF lookup and every other off-screen pass drew
// into the back buffer and left their targets unwritten. The cascade cache
// then reused the unwritten maps for as long as nothing changed. Only a
// readback can see it: the right targets are bound and the right draws
// submitted on that frame as on any other.
//
// Two frames reset the swapchain here. The first is the pipeline's first
// frame — r_vsync has left its boot value, as it does when a game loads
// its settings — with the whole scene already in place, which is how the
// defect was found: a still scene lit entirely as if occluded. The second
// is a frame mid-run on which the sun also swings round, so the cascades
// it needs are rendered on the very frame that is reset.
//
// After each, the untouched image must match the image with the cascades
// forced to render again.

#include "../gpu_scene_fixture.h"

#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

Entity g_sun = kInvalidEntity;

engine::runtime::LightComponent sun_shining_along_x(float sign) noexcept {
  engine::runtime::LightComponent sun{};
  sun.direction = engine::math::Vec3(sign, -0.6F, 0.0F);
  sun.intensity = 3.0F;
  return sun;
}

/// A floor, a two-metre cube at x = -3, and a sun shining along +x, so the
/// cube's shadow lies across the middle of the view. Seen from (0, 6, 10).
bool author_scene(World &world) noexcept {
  engine::runtime::Transform floorTransform{};
  floorTransform.scale = engine::math::Vec3(60.0F, 1.0F, 60.0F);
  engine::runtime::Transform cubeTransform{};
  cubeTransform.position = engine::math::Vec3(-3.0F, 1.0F, 0.0F);
  cubeTransform.scale = engine::math::Vec3(2.0F, 2.0F, 2.0F);
  g_sun = world.create_scene_object();
  return (engine::tests::add_builtin_mesh(
              world, "builtin://plane", floorTransform,
              engine::math::Vec3(0.8F, 0.8F, 0.8F)) != kInvalidEntity) &&
         (engine::tests::add_builtin_mesh(
              world, "builtin://cube", cubeTransform,
              engine::math::Vec3(0.8F, 0.8F, 0.8F)) != kInvalidEntity) &&
         (g_sun != kInvalidEntity) &&
         world.add_light_component(g_sun, sun_shining_along_x(1.0F)) &&
         engine::tests::look_from(world, engine::math::Vec3(0.0F, 6.0F, 10.0F),
                                  engine::math::Vec3(0.0F, 0.0F, 0.0F));
}

/// Where the scene's features land in the frame, as fractions of it. The
/// shadow is 3.3 m long on a floor strip 2 m deep; each rectangle sits a
/// good half metre inside its feature. The open floor is the bottom
/// quarter of the frame: floor between the camera and the cube's row,
/// which no shadow reaches under either sun.
struct Region final {
  double x0, y0, x1, y1;
};
constexpr Region kShadowWithSunAlongPlusX{0.44, 0.48, 0.53, 0.53};
constexpr Region kShadowWithSunAlongMinusX{0.22, 0.48, 0.30, 0.53};
constexpr Region kOpenFloor{0.0, 0.75, 1.0, 1.0};

double region_level(const CapturedFrame &frame, const Region &region) noexcept {
  const auto w = static_cast<double>(frame.width);
  const auto h = static_cast<double>(frame.height);
  return engine::tests::mean_channel(
      frame, 1U, static_cast<std::uint32_t>(region.x0 * w),
      static_cast<std::uint32_t>(region.y0 * h),
      static_cast<std::uint32_t>(region.x1 * w),
      static_cast<std::uint32_t>(region.y1 * h));
}

/// Captures the scene as it stands, then again with the cascades rendered
/// afresh, and holds the two to each other and to where the shadow must
/// be. 0 when they agree; a device with no readback sets *skipped.
int check_against_rerender(engine::EnginePipeline &pipeline, const char *label,
                           const char *untouchedPath, const char *rerenderPath,
                           const Region &shadow, bool *skipped) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::checked;
  using engine::tests::mean_abs_difference;
  using engine::tests::settle_frames;

  CapturedFrame untouched{};
  if (!settle_frames(pipeline, 12) ||
      !capture_presented_frame(pipeline, untouchedPath, &untouched)) {
    *skipped = true;
    return 0;
  }
  CapturedFrame untouchedAgain{};
  if (!settle_frames(pipeline) ||
      !capture_presented_frame(pipeline, "first_frame_again.tga",
                               &untouchedAgain)) {
    return 1;
  }
  checked(engine::core::cvar_set_bool("r_shadow_cache", false),
          "r_shadow_cache");
  CapturedFrame rerendered{};
  const bool captured =
      settle_frames(pipeline) &&
      capture_presented_frame(pipeline, rerenderPath, &rerendered);
  checked(engine::core::cvar_set_bool("r_shadow_cache", true),
          "r_shadow_cache");
  if (!captured || !settle_frames(pipeline)) {
    return 2;
  }

  const std::uint32_t w = untouched.width;
  const std::uint32_t h = untouched.height;
  const double noise =
      mean_abs_difference(untouched, untouchedAgain, 0U, 0U, w, h);
  const double drift = mean_abs_difference(untouched, rerendered, 0U, 0U, w, h);
  const double open = region_level(untouched, kOpenFloor);
  const double shadowed = region_level(untouched, shadow);
  const double openRerendered = region_level(rerendered, kOpenFloor);
  const double shadowedRerendered = region_level(rerendered, shadow);
  std::printf("first_frame_shadows_gpu_test: %s: untouched vs re-rendered "
              "%.3f levels (frame-to-frame %.3f); open floor %.2f, shadow "
              "%.2f; re-rendered %.2f and %.2f\n",
              label, drift, noise, open, shadowed, openRerendered,
              shadowedRerendered);

  int result = 0;
  // The reference has to hold a real shadow where the sun puts it, or the
  // comparison below would pass on a scene with none. The open floor reads
  // 197 here and the floor under the cube's shadow 146; with the shadow
  // missing the two agree to a level, so 40 separates the cases whatever
  // a driver does to the last few levels.
  if ((openRerendered - shadowedRerendered) < 40.0) {
    std::fprintf(stderr, "FAIL: %s: the re-rendered scene has no shadow "
                         "where the cube's should fall (%.2f against %.2f "
                         "open)\n",
                 label, shadowedRerendered, openRerendered);
    result = 3;
  }
  // Same scene, same camera, same sun: rendering the cascades again may
  // change nothing beyond what two untouched frames differ by, plus half a
  // level of readback rounding.
  if (drift > (noise + 0.5)) {
    std::fprintf(stderr, "FAIL: %s: the image differs from a re-render of "
                         "the same scene by %.3f levels; the cascades in "
                         "use are not the ones this scene renders\n",
                 label, drift);
    result = 4;
  }
  return result;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::checked;

  // None of these is part of the cascade cache key, so the maps the first
  // frame rendered are still the ones in use.
  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");

  bool skipped = false;
  const int firstFrame = check_against_rerender(
      pipeline, "scene whole on the first frame", "first_frame_untouched.tga",
      "first_frame_rerendered.tga", kShadowWithSunAlongPlusX, &skipped);
  if (skipped) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  // The sun swings to the other side on a frame that also resets the
  // swapchain. When the new direction reaches the cascade pass is the
  // pipeline's business, so each of the next frames resets as well; the
  // last toggle leaves the run unthrottled again.
  if (!world.remove_light_component(g_sun) ||
      !world.add_light_component(g_sun, sun_shining_along_x(-1.0F))) {
    return 10;
  }
  for (int toggle = 0; toggle < 4; ++toggle) {
    checked(engine::core::cvar_set_int("r_vsync", ((toggle % 2) == 0) ? 1 : 0),
            "r_vsync");
    if (!pipeline.execute_frame()) {
      return 11;
    }
  }
  const int midRun = check_against_rerender(
      pipeline, "sun moved on a frame that reset", "first_frame_moved.tga",
      "first_frame_moved_rerendered.tga", kShadowWithSunAlongMinusX, &skipped);

  if (firstFrame != 0) {
    return 20 + firstFrame;
  }
  return (midRun != 0) ? (30 + midRun) : 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("first_frame_shadows_gpu_test", &run,
                                           &author_scene);
}
