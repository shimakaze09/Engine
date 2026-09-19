// GPU regression for issue #565 row 5: the deferred pass's tiled-light
// table may wrap onto texture rows shorter than the screen's tile-column
// count, and the cooked shader must address the wrapped table exactly as
// it addresses the unwrapped one. Drives the production EnginePipeline on
// the real bgfx device and compares presented frames.
//
// Two cases. The first lowers the limit the layout is given, through
// r_tile_table_max_dimension, so the table wraps at the window's own size
// and the frame can be compared with the unwrapped one pixel for pixel.
// The second is the reported one: a 7680x2160 scene, 480 tile columns,
// whose unwrapped table would be 24000 texels wide. No display here is
// that wide, but the passes are sized by set_scene_viewport_size — what
// the editor's viewport panel drives — so the real size is reachable. On
// base the table failed to exist at that size wherever the texture limit
// is 16384 (D3D11, D3D12) and the floor went dark; the suite registers a
// D3D11 run on Windows for that reason.
// Two point lights of different colours sit in different tiles, so a wrong
// row or column lights the wrong part of the floor, or none of it.

#include "../gpu_scene_fixture.h"

#include "engine/renderer/command_buffer.h"

#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

/// The least the two point lights must change the floor by, in 8-bit
/// levels averaged over it, for them to count as rendered. Without the tile
/// table the lit and unlit frames are the same frame and the difference is
/// exactly the frame-to-frame one, which is zero here; with it the lights
/// are worth 8 to 18 levels depending on how much of the view their pools
/// cover. Two levels is four times the half level allowed for readback
/// rounding, and a quarter of the smallest real contribution.
constexpr double kMinLightContribution = 2.0;

engine::runtime::PointLightComponent pool_light(float r, float g) noexcept {
  engine::runtime::PointLightComponent light{};
  light.color = engine::math::Vec3(r, g, 0.05F);
  light.intensity = 12.0F;
  light.radius = 7.0F;
  return light;
}

/// A white floor seen from above and in front, lit almost only by a red
/// point light left of centre and a green one right of it, so what they
/// contribute is most of the image.
bool build_scene(World &world, Entity *outRed, Entity *outGreen) noexcept {
  engine::runtime::Transform floorTransform{};
  floorTransform.scale = engine::math::Vec3(40.0F, 1.0F, 40.0F);
  if (engine::tests::add_builtin_mesh(world, "builtin://plane", floorTransform,
                                      engine::math::Vec3(1.0F, 1.0F, 1.0F)) ==
      kInvalidEntity) {
    return false;
  }

  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.intensity = 0.05F;
  if ((sun == kInvalidEntity) || !world.add_light_component(sun, sunLight)) {
    return false;
  }

  engine::runtime::Transform redTransform{};
  redTransform.position = engine::math::Vec3(-4.0F, 1.5F, 0.0F);
  *outRed = world.create_scene_object(redTransform);
  engine::runtime::Transform greenTransform{};
  greenTransform.position = engine::math::Vec3(4.0F, 1.5F, 0.0F);
  *outGreen = world.create_scene_object(greenTransform);
  if ((*outRed == kInvalidEntity) || (*outGreen == kInvalidEntity) ||
      !world.add_point_light_component(*outRed, pool_light(1.0F, 0.05F)) ||
      !world.add_point_light_component(*outGreen, pool_light(0.05F, 1.0F))) {
    return false;
  }
  return engine::tests::look_from(world, engine::math::Vec3(0.0F, 9.0F, 11.0F),
                                  engine::math::Vec3(0.0F, 0.0F, 0.0F));
}

/// Mean red and green over the left and right halves of the floor, which
/// fills the lower two thirds of the view.
struct FloorSides final {
  double redLeft = 0.0;
  double greenLeft = 0.0;
  double redRight = 0.0;
  double greenRight = 0.0;

  bool red_is_left_and_green_is_right() const noexcept {
    return (redLeft > greenLeft) && (greenRight > redRight);
  }
};

FloorSides measure_sides(const CapturedFrame &frame) noexcept {
  const std::uint32_t top = frame.height / 3U;
  const std::uint32_t mid = frame.width / 2U;
  FloorSides sides{};
  sides.redLeft = engine::tests::mean_channel(frame, 2U, 0U, top, mid,
                                              frame.height);
  sides.greenLeft = engine::tests::mean_channel(frame, 1U, 0U, top, mid,
                                                frame.height);
  sides.redRight = engine::tests::mean_channel(frame, 2U, mid, top,
                                               frame.width, frame.height);
  sides.greenRight = engine::tests::mean_channel(frame, 1U, mid, top,
                                                 frame.width, frame.height);
  return sides;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::mean_abs_difference;
  using engine::tests::settle_frames;

  static_cast<void>(
      engine::core::cvar_set_int("r_tile_table_max_dimension", 0));
  Entity red = kInvalidEntity;
  Entity green = kInvalidEntity;
  if (!build_scene(world, &red, &green)) {
    return 10;
  }
  if (!settle_frames(pipeline, 20)) {
    return 11;
  }

  CapturedFrame unwrapped{};
  if (!capture_presented_frame(pipeline, "tile_wrap_a.tga", &unwrapped)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  const std::uint32_t w = unwrapped.width;
  const std::uint32_t h = unwrapped.height;

  // The G-buffer view exists only on the deferred path, so an image that
  // does not change when it is switched on was drawn by the forward path,
  // where there is no tile table to wrap.
  static_cast<void>(engine::core::cvar_set_int("r_gbuffer_debug", 1));
  CapturedFrame gbufferView{};
  if (!settle_frames(pipeline) ||
      !capture_presented_frame(pipeline, "tile_wrap_g.tga", &gbufferView)) {
    return 12;
  }
  static_cast<void>(engine::core::cvar_set_int("r_gbuffer_debug", 0));
  if (mean_abs_difference(unwrapped, gbufferView, 0U, 0U, w, h) < 4.0) {
    std::printf("SKIPPED: the deferred path is not active on this device\n");
    return 0;
  }

  // A second capture of the unchanged scene measures how much two frames
  // differ with nothing changed at all; the wrap may not add to that.
  CapturedFrame repeat{};
  if (!settle_frames(pipeline) ||
      !capture_presented_frame(pipeline, "tile_wrap_r.tga", &repeat)) {
    return 13;
  }

  // 512 texels hold ten 50-texel tiles, so a drawable's tile columns wrap
  // onto many short rows and both lights' tiles move off their old rows.
  static_cast<void>(
      engine::core::cvar_set_int("r_tile_table_max_dimension", 512));
  CapturedFrame wrapped{};
  if (!settle_frames(pipeline) ||
      !capture_presented_frame(pipeline, "tile_wrap_b.tga", &wrapped)) {
    return 14;
  }
  static_cast<void>(
      engine::core::cvar_set_int("r_tile_table_max_dimension", 0));

  if (!world.remove_point_light_component(red) ||
      !world.remove_point_light_component(green)) {
    return 15;
  }
  CapturedFrame unlit{};
  if (!settle_frames(pipeline) ||
      !capture_presented_frame(pipeline, "tile_wrap_c.tga", &unlit)) {
    return 16;
  }

  // The reported size, reached the way the editor's viewport reaches any
  // size. The unlit frame is taken first and the lights then come back,
  // so the two captures differ only by them.
  engine::renderer::set_scene_viewport_size(7680, 2160);
  CapturedFrame wideUnlit{};
  CapturedFrame wideLit{};
  const bool wideCaptured =
      settle_frames(pipeline) &&
      capture_presented_frame(pipeline, "tile_wrap_wu.tga", &wideUnlit) &&
      world.add_point_light_component(red, pool_light(1.0F, 0.05F)) &&
      world.add_point_light_component(green, pool_light(0.05F, 1.0F)) &&
      settle_frames(pipeline) &&
      capture_presented_frame(pipeline, "tile_wrap_wl.tga", &wideLit);
  engine::renderer::set_scene_viewport_size(0, 0);
  if (!wideCaptured) {
    return 17;
  }

  const std::uint32_t top = h / 3U;
  const double noise = mean_abs_difference(unwrapped, repeat, 0U, top, w, h);
  const double wrapDelta =
      mean_abs_difference(unwrapped, wrapped, 0U, top, w, h);
  const double lightDelta = mean_abs_difference(unwrapped, unlit, 0U, top, w, h);
  const FloorSides wrappedSides = measure_sides(wrapped);
  const double wideLightDelta =
      mean_abs_difference(wideLit, wideUnlit, 0U, wideLit.height / 3U,
                          wideLit.width, wideLit.height);
  const FloorSides wideSides = measure_sides(wideLit);

  std::printf("deferred_tile_wrap_gpu_test: %ux%u frame-to-frame=%.3f "
              "wrap=%.3f lights=%.3f | wrapped left R/G %.1f/%.1f right R/G "
              "%.1f/%.1f\n",
              w, h, noise, wrapDelta, lightDelta, wrappedSides.redLeft,
              wrappedSides.greenLeft, wrappedSides.redRight,
              wrappedSides.greenRight);
  std::printf("deferred_tile_wrap_gpu_test: 7680x2160 scene lights=%.3f | "
              "left R/G %.1f/%.1f right R/G %.1f/%.1f\n",
              wideLightDelta, wideSides.redLeft, wideSides.greenLeft,
              wideSides.redRight, wideSides.greenRight);

  int result = 0;
  // The lights have to be most of what the floor shows, or "the wrapped
  // frame matches" would hold for a frame with no local lights in either.
  if (lightDelta < kMinLightContribution) {
    std::fprintf(stderr, "FAIL: removing both point lights changed the floor "
                         "by only %.3f levels; the scene does not exercise "
                         "local lights\n",
                 lightDelta);
    result = 20;
  }
  // Wrapping changes where tile data sits, not what it holds, so the image
  // may differ from the unwrapped one by no more than two unchanged frames
  // differ from each other, plus half a level for readback rounding.
  if (wrapDelta > (noise + 0.5)) {
    std::fprintf(stderr, "FAIL: the wrapped tile table changed the image by "
                         "%.3f levels (frame-to-frame %.3f)\n",
                 wrapDelta, noise);
    result = 21;
  }
  if (!wrappedSides.red_is_left_and_green_is_right()) {
    std::fprintf(stderr, "FAIL: the wrapped table lit the wrong side of the "
                         "floor\n");
    result = 22;
  }
  // At the reported size the local lights must still be there, on the
  // right sides.
  if (wideLightDelta < kMinLightContribution) {
    std::fprintf(stderr, "FAIL: at 7680x2160 the point lights changed the "
                         "floor by only %.3f levels; the deferred local "
                         "lights did not survive that size\n",
                 wideLightDelta);
    result = 23;
  }
  if (!wideSides.red_is_left_and_green_is_right()) {
    std::fprintf(stderr, "FAIL: at 7680x2160 the wrapped table lit the wrong "
                         "side of the floor\n");
    result = 24;
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("deferred_tile_wrap_gpu_test", &run);
}
