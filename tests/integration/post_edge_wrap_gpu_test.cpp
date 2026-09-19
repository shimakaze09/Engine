// GPU regression for issue #565 row 1: the post chain's targets clamp at
// their edges. They were created with a repeating wrap, so a bloom or FXAA
// tap that reached past one screen edge read the opposite one, and a
// bright object at the left of the view glowed on the right. Wrap mode is
// a property of the sampler the GPU uses; no CPU-side suite can see what a
// tap past the edge returns.
//
// A white slab under a strong light sits against the left edge of the view
// and nothing else is lit. The strip of pixels along the right edge must
// look the same whether the slab is there or not.

#include "../gpu_scene_fixture.h"

#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::mean_abs_difference;
  using engine::tests::settle_frames;

  engine::tests::checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  engine::tests::checked(engine::core::cvar_set_bool("r_bloom", true), "r_bloom");
  engine::tests::checked(engine::core::cvar_set_bool("r_fxaa", true), "r_fxaa");
  // Default intensity keeps a leak near one level; this makes a wrapped tap
  // worth many, without changing where taps land.
  engine::tests::checked(engine::core::cvar_set_float("r_bloom_intensity", 2.0F), "r_bloom_intensity");
  engine::tests::checked(engine::core::cvar_set_float("r_bloom_threshold", 0.8F), "r_bloom_threshold");

  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.intensity = 0.02F;
  if ((sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 10.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  CapturedFrame empty{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "edge_empty.tga", &empty)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  CapturedFrame emptyAgain{};
  if (!settle_frames(pipeline) ||
      !capture_presented_frame(pipeline, "edge_empty2.tga", &emptyAgain)) {
    return 11;
  }

  // At ten metres a 60 degree view is about 10.3 metres half-wide at 16:9,
  // so a slab centred on x = -10 straddles the left edge: its bright face
  // runs right up to the border the taps would cross.
  engine::runtime::Transform slabTransform{};
  slabTransform.position = engine::math::Vec3(-10.0F, 0.0F, 0.0F);
  slabTransform.scale = engine::math::Vec3(3.0F, 9.0F, 0.5F);
  engine::runtime::Transform lampTransform{};
  lampTransform.position = engine::math::Vec3(-9.0F, 0.0F, 3.0F);
  const Entity lamp = world.create_scene_object(lampTransform);
  engine::runtime::PointLightComponent lampLight{};
  lampLight.intensity = 60.0F;
  lampLight.radius = 8.0F;
  if ((engine::tests::add_builtin_mesh(world, "builtin://cube", slabTransform,
                                       engine::math::Vec3(1.0F, 1.0F, 1.0F)) ==
       kInvalidEntity) ||
      (lamp == kInvalidEntity) ||
      !world.add_point_light_component(lamp, lampLight)) {
    return 12;
  }
  CapturedFrame bright{};
  if (!settle_frames(pipeline, 10) ||
      !capture_presented_frame(pipeline, "edge_bright.tga", &bright)) {
    return 13;
  }

  const std::uint32_t w = bright.width;
  const std::uint32_t h = bright.height;
  const std::uint32_t strip = w / 32U;
  const double noise =
      mean_abs_difference(empty, emptyAgain, w - strip, 0U, w, h);
  const double leftGlow = mean_abs_difference(empty, bright, 0U, 0U, strip, h);
  const double rightLeak =
      mean_abs_difference(empty, bright, w - strip, 0U, w, h);
  std::printf("post_edge_wrap_gpu_test: %ux%u left edge changed by %.3f "
              "levels, right edge by %.3f (frame-to-frame %.3f)\n",
              w, h, leftGlow, rightLeak, noise);

  int result = 0;
  // The slab has to be bright at the border, or there is nothing to leak.
  if (leftGlow < 20.0) {
    std::fprintf(stderr, "FAIL: the slab changed the left edge by only %.3f "
                         "levels; the scene puts nothing bright at the "
                         "border\n",
                 leftGlow);
    result = 20;
  }
  // Nothing on the right changed, so the right edge may differ by no more
  // than two unchanged frames do, plus half a level of readback rounding.
  if (rightLeak > (noise + 0.5)) {
    std::fprintf(stderr, "FAIL: a bright object at the left edge changed the "
                         "right edge by %.3f levels; the post chain wraps\n",
                 rightLeak);
    result = 21;
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("post_edge_wrap_gpu_test", &run);
}
