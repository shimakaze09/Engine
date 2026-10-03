// GPU check for issue #687: the deferred path draws the procedural sky
// with the same uniforms the forward path does. On Direct3D 12 the
// deferred sky was reported dark blue-grey and deaf to r_sky_turbidity
// while the forward sky was right.
//
// The camera looks up into an empty sky. The same view is drawn deferred
// and forward and must agree; then the turbidity changes and the deferred
// sky must change with it, as the forward sky does. Fog, bloom, SSAO and
// automatic exposure are off, so nothing but the sky reaches the pixels.

#include "../gpu_scene_fixture.h"

#include <cmath>
#include <cstdio>

namespace {

using engine::tests::CapturedFrame;
using engine::tests::checked;

/// Mean of each colour channel over the frame's upper middle, which the
/// upward camera fills with sky.
struct SkyColour final {
  double rgb[3] = {};
};

SkyColour sky_colour(const CapturedFrame &frame) noexcept {
  SkyColour colour{};
  const std::uint32_t x0 = frame.width / 4U;
  const std::uint32_t x1 = (frame.width * 3U) / 4U;
  const std::uint32_t y0 = frame.height / 8U;
  const std::uint32_t y1 = frame.height / 2U;
  double count = 0.0;
  for (std::uint32_t y = y0; y < y1; ++y) {
    for (std::uint32_t x = x0; x < x1; ++x) {
      // The capture is BGRA; report RGB.
      colour.rgb[0] += frame.channel(x, y, 2U);
      colour.rgb[1] += frame.channel(x, y, 1U);
      colour.rgb[2] += frame.channel(x, y, 0U);
      count += 1.0;
    }
  }
  for (double &channel : colour.rgb) {
    channel /= (count > 0.0) ? count : 1.0;
  }
  return colour;
}

double largest_difference(const SkyColour &a, const SkyColour &b) noexcept {
  double largest = 0.0;
  for (int c = 0; c < 3; ++c) {
    largest = std::fmax(largest, std::fabs(a.rgb[c] - b.rgb[c]));
  }
  return largest;
}

bool capture_sky(engine::EnginePipeline &pipeline, const char *path,
                 SkyColour *out) noexcept {
  CapturedFrame frame{};
  if (!engine::tests::settle_frames(pipeline, 12) ||
      !engine::tests::capture_presented_frame(pipeline, path, &frame)) {
    return false;
  }
  *out = sky_colour(frame);
  return true;
}

void print(const char *what, const SkyColour &c) noexcept {
  std::printf("deferred_sky_gpu_test: %s sky %.1f %.1f %.1f\n", what, c.rgb[0],
              c.rgb[1], c.rgb[2]);
}

int run(engine::EnginePipeline &pipeline,
        engine::runtime::World &world) noexcept {
  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_bool("r_auto_exposure", false),
          "r_auto_exposure");
  checked(engine::core::cvar_set_string("r_sky_model", "hosek"), "r_sky_model");
  // Looking well above the horizon, so the frame is all sky.
  if (!engine::tests::look_from(world, engine::math::Vec3(0.0F, 1.0F, 0.0F),
                                engine::math::Vec3(0.0F, 3.0F, -4.0F))) {
    return 10;
  }

  SkyColour deferredClear{};
  SkyColour forwardClear{};
  SkyColour deferredHazy{};
  SkyColour forwardHazy{};
  const bool captured =
      engine::core::cvar_set_float("r_sky_turbidity", 2.0F) &&
      engine::core::cvar_set_bool("r_deferred", true) &&
      capture_sky(pipeline, "deferred_sky_clear.tga", &deferredClear) &&
      engine::core::cvar_set_bool("r_deferred", false) &&
      capture_sky(pipeline, "forward_sky_clear.tga", &forwardClear) &&
      engine::core::cvar_set_float("r_sky_turbidity", 8.0F) &&
      capture_sky(pipeline, "forward_sky_hazy.tga", &forwardHazy) &&
      engine::core::cvar_set_bool("r_deferred", true) &&
      capture_sky(pipeline, "deferred_sky_hazy.tga", &deferredHazy);
  checked(engine::core::cvar_set_float("r_sky_turbidity", 3.0F),
          "r_sky_turbidity");
  if (!captured) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  print("deferred, turbidity 2:", deferredClear);
  print("forward, turbidity 2:", forwardClear);
  print("deferred, turbidity 8:", deferredHazy);
  print("forward, turbidity 8:", forwardHazy);

  int result = 0;
  // The positive control: turbidity visibly changes the forward sky.
  // Twenty levels is far above any rounding between two draws of one sky
  // and far below what a change from clear to hazy does.
  constexpr double kVisibleChange = 20.0;
  if (largest_difference(forwardClear, forwardHazy) < kVisibleChange) {
    std::fprintf(stderr, "FAIL: turbidity does not change the forward sky; "
                         "the test cannot tell the paths apart\n");
    return 11;
  }
  // The two paths draw one sky. Three levels allows the deferred path's
  // extra target copy and the 8-bit rounding of each capture.
  constexpr double kSameSky = 3.0;
  if (largest_difference(deferredClear, forwardClear) > kSameSky ||
      largest_difference(deferredHazy, forwardHazy) > kSameSky) {
    std::fprintf(stderr,
                 "FAIL: the deferred sky differs from the forward sky\n");
    result = 12;
  }
  if (largest_difference(deferredClear, deferredHazy) < kVisibleChange) {
    std::fprintf(stderr, "FAIL: the deferred sky ignores r_sky_turbidity\n");
    result = 13;
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("deferred_sky_gpu_test", &run);
}
