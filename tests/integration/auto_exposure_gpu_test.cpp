// Verifies on a real render device that auto exposure adapts to the scene:
// a floor and a cube under a sun, captured with the sun at one and at eight
// times that intensity. With r_auto_exposure off the brighter sun reads
// much brighter; with it on, the adapted exposure brings both back toward
// middle grey, so the two frames read close together, and r_exposure still
// brightens the adapted frame as its compensation. Until the adaptation
// pass existed the chain's average was never read and the exposure stayed
// 1, so auto exposure changed nothing.

#include "../gpu_scene_fixture.h"

#include "engine/math/transform.h"

#include <cmath>
#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

/// Mean 8-bit level over every channel of every pixel.
double frame_level(const CapturedFrame &frame) noexcept {
  double sum = 0.0;
  for (std::uint32_t y = 0U; y < frame.height; ++y) {
    for (std::uint32_t x = 0U; x < frame.width; ++x) {
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        sum += frame.channel(x, y, c);
      }
    }
  }
  return sum / (static_cast<double>(frame.width) * frame.height * 3.0);
}

/// Sets the sun's intensity, restarts auto exposure when it is on so the
/// capture shows the exposure adapted to this scene rather than one still
/// easing from the last, and captures the settled frame.
bool capture_level(engine::EnginePipeline &pipeline, World &world, Entity sun,
                   float intensity, bool autoExposure, float exposure,
                   const char *name, double *outLevel) noexcept {
  engine::runtime::LightComponent light{};
  light.direction = engine::math::Vec3(0.3F, -1.0F, 0.2F);
  light.intensity = intensity;
  engine::tests::checked(engine::core::cvar_set_float("r_exposure", exposure),
                         "r_exposure");
  engine::tests::checked(engine::core::cvar_set_bool("r_auto_exposure", false),
                         "r_auto_exposure");
  if (!world.add_light_component(sun, light) ||
      !engine::tests::settle_frames(pipeline, 2)) {
    return false;
  }
  engine::tests::checked(
      engine::core::cvar_set_bool("r_auto_exposure", autoExposure),
      "r_auto_exposure");
  CapturedFrame frame{};
  if (!engine::tests::settle_frames(pipeline, 8) ||
      !engine::tests::capture_presented_frame(pipeline, name, &frame)) {
    return false;
  }
  *outLevel = frame_level(frame);
  std::printf("%s: sun %.1f, auto %d, exposure %.1f -> level %.2f\n", name,
              static_cast<double>(intensity), autoExposure ? 1 : 0,
              static_cast<double>(exposure), *outLevel);
  return true;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::checked;
  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_height_fog", false), "r_height_fog");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  // An adaptation speed this high makes every blend step reach the target,
  // so the settled frame does not depend on how long the frames took.
  checked(engine::core::cvar_set_float("r_auto_exposure_speed", 1000.0F),
          "r_auto_exposure_speed");

  engine::runtime::Transform cubeTransform{};
  cubeTransform.position = engine::math::Vec3(0.0F, 0.5F, 0.0F);
  const Entity floor = engine::tests::add_builtin_mesh(
      world, "builtin://plane", engine::tests::framed_floor_transform(20.0F),
      engine::math::Vec3(0.8F, 0.8F, 0.8F));
  const Entity cube =
      engine::tests::add_builtin_mesh(world, "builtin://cube", cubeTransform,
                                      engine::math::Vec3(0.8F, 0.8F, 0.8F));
  const Entity sun = world.create_scene_object();
  if ((floor == kInvalidEntity) || (cube == kInvalidEntity) ||
      (sun == kInvalidEntity) ||
      !engine::tests::look_from(world, engine::math::Vec3(2.0F, 3.0F, 5.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  double dimManual = 0.0;
  double brightManual = 0.0;
  double dimAuto = 0.0;
  double brightAuto = 0.0;
  double dimAutoCompensated = 0.0;
  if (!capture_level(pipeline, world, sun, 1.0F, false, 1.0F,
                     "auto_exposure_dim_manual.tga", &dimManual)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  if (!capture_level(pipeline, world, sun, 8.0F, false, 1.0F,
                     "auto_exposure_bright_manual.tga", &brightManual) ||
      !capture_level(pipeline, world, sun, 1.0F, true, 1.0F,
                     "auto_exposure_dim_auto.tga", &dimAuto) ||
      !capture_level(pipeline, world, sun, 8.0F, true, 1.0F,
                     "auto_exposure_bright_auto.tga", &brightAuto) ||
      !capture_level(pipeline, world, sun, 1.0F, true, 2.0F,
                     "auto_exposure_dim_auto_x2.tga", &dimAutoCompensated)) {
    return 11;
  }

  const double manualSpread = brightManual - dimManual;
  const double autoSpread = std::fabs(brightAuto - dimAuto);
  if (manualSpread < 20.0) {
    std::fprintf(stderr,
                 "FAIL: an eight times brighter sun raised the manual frame "
                 "by only %.2f levels; the scene does not test adaptation\n",
                 manualSpread);
    return 12;
  }
  // Adaptation normalizes the scene's average luminance to one key, so
  // the two frames would read alike but for the parts of the frame the
  // sun does not light (the sky) and the tonemap curve's shoulder. A
  // quarter of the manual spread leaves room for those and still fails
  // an exposure that does not move, which reproduces the whole spread.
  if (autoSpread > manualSpread / 4.0) {
    std::fprintf(stderr,
                 "FAIL: auto exposure left %.2f of the manual %.2f levels "
                 "between the dim and the bright sun\n",
                 autoSpread, manualSpread);
    return 13;
  }
  // Doubling the compensation doubles the adapted exposure, which must
  // show; four levels is well clear of readback rounding.
  if (dimAutoCompensated - dimAuto < 4.0) {
    std::fprintf(stderr,
                 "FAIL: r_exposure 2 raised the adapted frame by only %.2f "
                 "levels\n",
                 dimAutoCompensated - dimAuto);
    return 14;
  }
  return 0;
}

} // namespace

int main() {
  return engine::tests::run_gpu_scene_test("auto_exposure_gpu_test", &run);
}
