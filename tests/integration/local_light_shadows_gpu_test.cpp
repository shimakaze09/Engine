// GPU regression for issue #522: a point light and a spot light whose
// castShadow is set put a shadow in the image. On base nothing ever set
// the flag, so the spot and point depth passes had no candidates and every
// local light shone straight through geometry. The fake-device suite
// proves the depth passes are issued for a flagged light; only the image
// shows that the lighting pass then samples them where it should: a cube
// hangs between the light and a white floor, and the floor under it must
// be darker with the flag set than without.

#include "../gpu_scene_fixture.h"

#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

/// Pixels that are clearly darker with the flag set than without it. A
/// shadow edge is soft, so its core is what counts: fifteen levels is far
/// above the zero two unchanged frames differ by, and well under the
/// fifty or so the core of a local light's shadow loses.
std::uint32_t count_darkened(const CapturedFrame &lit,
                             const CapturedFrame &shadowed) noexcept {
  constexpr int kDarker = 15;
  if ((lit.width != shadowed.width) || (lit.height != shadowed.height)) {
    return 0U;
  }
  std::uint32_t count = 0U;
  for (std::uint32_t y = 0U; y < lit.height; ++y) {
    for (std::uint32_t x = 0U; x < lit.width; ++x) {
      int drop = 0;
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        drop += static_cast<int>(lit.channel(x, y, c)) -
                static_cast<int>(shadowed.channel(x, y, c));
      }
      if (drop > (3 * kDarker)) {
        ++count;
      }
    }
  }
  return count;
}

/// Captures the scene with the light's flag off and then on, and reports
/// how many pixels the shadow darkened. Negative on failure.
template <typename SetFlag>
int shadow_pixels(engine::EnginePipeline &pipeline, const char *label,
                  const char *offPath, const char *onPath,
                  SetFlag &&setFlag) noexcept {
  CapturedFrame off{};
  CapturedFrame on{};
  if (!setFlag(false) || !engine::tests::settle_frames(pipeline, 10) ||
      !engine::tests::capture_presented_frame(pipeline, offPath, &off) ||
      !setFlag(true) || !engine::tests::settle_frames(pipeline, 10) ||
      !engine::tests::capture_presented_frame(pipeline, onPath, &on)) {
    return -1;
  }
  const std::uint32_t darkened = count_darkened(off, on);
  const std::uint32_t brightened = count_darkened(on, off);
  std::printf("local_light_shadows_gpu_test: %s castShadow darkened %u "
              "pixels of %ux%u and brightened %u\n",
              label, darkened, off.width, off.height, brightened);
  return static_cast<int>(darkened);
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  engine::tests::checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  const engine::runtime::Transform floorTransform =
      engine::tests::framed_floor_transform(40.0F);
  engine::runtime::Transform cubeTransform{};
  cubeTransform.position = engine::math::Vec3(0.0F, 1.5F, 0.0F);
  cubeTransform.scale = engine::math::Vec3(2.0F, 0.5F, 2.0F);
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.intensity = 0.05F;
  if ((engine::tests::add_builtin_mesh(world, "builtin://plane", floorTransform,
                                       engine::math::Vec3(1.0F, 1.0F, 1.0F)) ==
       kInvalidEntity) ||
      (engine::tests::add_builtin_mesh(world, "builtin://cube", cubeTransform,
                                       engine::math::Vec3(0.8F, 0.8F, 0.8F)) ==
       kInvalidEntity) ||
      (sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 7.0F, 9.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  // Directly above the cube, so its shadow falls straight down onto the
  // floor beneath it.
  engine::runtime::Transform lightTransform{};
  lightTransform.position = engine::math::Vec3(0.0F, 5.0F, 0.0F);
  const Entity lamp = world.create_scene_object(lightTransform);
  if (lamp == kInvalidEntity) {
    return 11;
  }

  engine::runtime::PointLightComponent point{};
  point.intensity = 5.0F;
  point.radius = 14.0F;
  if (!world.add_point_light_component(lamp, point)) {
    return 12;
  }
  CapturedFrame probe{};
  if (!engine::tests::settle_frames(pipeline, 20) ||
      !engine::tests::capture_presented_frame(pipeline, "shadow_probe.tga",
                                              &probe)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  const int pointPixels = shadow_pixels(
      pipeline, "point light", "shadow_point_off.tga", "shadow_point_on.tga",
      [&](bool cast) noexcept {
        point.castShadow = cast;
        return world.remove_point_light_component(lamp) &&
               world.add_point_light_component(lamp, point);
      });
  if (!world.remove_point_light_component(lamp)) {
    return 13;
  }

  engine::runtime::SpotLightComponent spot{};
  spot.direction = engine::math::Vec3(0.0F, -1.0F, 0.0F);
  spot.intensity = 7.0F;
  spot.radius = 14.0F;
  spot.innerConeAngle = 0.6F;
  spot.outerConeAngle = 0.8F;
  if (!world.add_spot_light_component(lamp, spot)) {
    return 14;
  }
  const int spotPixels = shadow_pixels(
      pipeline, "spot light", "shadow_spot_off.tga", "shadow_spot_on.tga",
      [&](bool cast) noexcept {
        spot.castShadow = cast;
        return world.remove_spot_light_component(lamp) &&
               world.add_spot_light_component(lamp, spot);
      });

  // A light that ignores the flag leaves the two frames identical and
  // darkens nothing. The slab is two metres square a few metres from the
  // camera, so its shadow covers several thousand pixels; a thousand
  // leaves room for another window size.
  constexpr int kMinShadowPixels = 1000;
  int result = 0;
  if (pointPixels < kMinShadowPixels) {
    std::fprintf(stderr, "FAIL: the point light's castShadow darkened only "
                         "%d pixels\n",
                 pointPixels);
    result = 20;
  }
  if (spotPixels < kMinShadowPixels) {
    std::fprintf(stderr, "FAIL: the spot light's castShadow darkened only "
                         "%d pixels\n",
                 spotPixels);
    result = 21;
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("local_light_shadows_gpu_test",
                                           &run);
}
