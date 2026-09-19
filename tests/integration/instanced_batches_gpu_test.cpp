// GPU regression for issue #523: two instance-compatible batches drawn in
// one frame each land at their own positions. Every instanced batch
// uploads its matrices to the same engine stream buffer before its draw;
// on base the backend realized that as one bgfx dynamic buffer, whose
// updates all run before any of the frame's draws, so every batch drew
// with the last batch's matrices. The fake-device suites record that each
// batch bound instance data, never which matrices the GPU read, so only
// the image shows it: three red cubes on the left and three green spheres
// on the right must both be there. With the defect one group is drawn on
// top of the other and its own side is empty.

#include "../gpu_scene_fixture.h"

#include <cstdio>

namespace {

using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

/// What one half of the view holds that the empty scene did not: how many
/// pixels changed, and whether those pixels lean red or green. A red cube
/// reads about a hundred levels redder than green and a green sphere about
/// as much the other way, so the sign of the lean names the batch with no
/// threshold to tune.
struct HalfContent final {
  std::uint32_t changedPixels = 0U;
  double redMinusGreen = 0.0;
};

HalfContent measure_half(const CapturedFrame &scene, const CapturedFrame &empty,
                         std::uint32_t x0, std::uint32_t x1) noexcept {
  // A pixel an object covers differs from the empty sky or ground by tens
  // of levels; twelve is far above the zero two unchanged frames differ by.
  constexpr int kChanged = 12;
  HalfContent content{};
  if ((scene.width != empty.width) || (scene.height != empty.height)) {
    return content;
  }
  std::int64_t lean = 0;
  for (std::uint32_t y = 0U; y < scene.height; ++y) {
    for (std::uint32_t x = x0; x < x1; ++x) {
      int delta = 0;
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        const int d = static_cast<int>(scene.channel(x, y, c)) -
                      static_cast<int>(empty.channel(x, y, c));
        delta += (d < 0) ? -d : d;
      }
      if (delta > (3 * kChanged)) {
        ++content.changedPixels;
        lean += static_cast<int>(scene.channel(x, y, 2U)) -
                static_cast<int>(scene.channel(x, y, 1U));
      }
    }
  }
  if (content.changedPixels > 0U) {
    content.redMinusGreen = static_cast<double>(lean) /
                            static_cast<double>(content.changedPixels);
  }
  return content;
}

/// Three of one mesh with one colour in a row along x: same mesh and
/// material, so render prep merges them into a single instanced batch.
bool add_row(World &world, const char *mesh, float firstX,
             const engine::math::Vec3 &albedo) noexcept {
  for (int i = 0; i < 3; ++i) {
    engine::runtime::Transform transform{};
    transform.position = engine::math::Vec3(
        firstX + (1.6F * static_cast<float>(i)), 0.5F, 0.0F);
    if (engine::tests::add_builtin_mesh(world, mesh, transform, albedo) ==
        kInvalidEntity) {
      return false;
    }
  }
  return true;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::settle_frames;

  // Distance fog would fade both groups toward the same grey.
  engine::tests::checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  const engine::runtime::Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.direction = engine::math::Vec3(-0.2F, -0.6F, -1.0F);
  sunLight.intensity = 3.0F;
  if ((sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 2.0F, 9.0F),
                                engine::math::Vec3(0.0F, 0.5F, 0.0F))) {
    return 10;
  }

  // Both render paths batch instances, and each has its own upload site.
  // The empty scene is captured per path first, so what changes afterwards
  // is the objects alone.
  const bool deferredModes[2] = {true, false};
  CapturedFrame empty[2]{};
  for (int mode = 0; mode < 2; ++mode) {
    engine::tests::checked(engine::core::cvar_set_bool("r_deferred", deferredModes[mode]), "r_deferred");
    if (!settle_frames(pipeline, 20) ||
        !capture_presented_frame(pipeline, "instanced_empty.tga",
                                 &empty[mode])) {
      if (mode == 0) {
        std::printf("SKIPPED: the device returned no back-buffer readback\n");
        return 0;
      }
      return 11;
    }
  }

  if (!add_row(world, "builtin://cube", -6.2F,
               engine::math::Vec3(1.0F, 0.02F, 0.02F)) ||
      !add_row(world, "builtin://sphere", 3.0F,
               engine::math::Vec3(0.02F, 1.0F, 0.02F))) {
    return 12;
  }

  int result = 0;
  for (int mode = 0; mode < 2; ++mode) {
    const bool deferred = deferredModes[mode];
    const char *label = deferred ? "deferred" : "forward";
    engine::tests::checked(engine::core::cvar_set_bool("r_deferred", deferred), "r_deferred");
    CapturedFrame frame{};
    if (!settle_frames(pipeline, 20) ||
        !capture_presented_frame(pipeline,
                                 deferred ? "instanced_deferred.tga"
                                          : "instanced_forward.tga",
                                 &frame)) {
      return 13;
    }
    const std::uint32_t mid = frame.width / 2U;
    const HalfContent left = measure_half(frame, empty[mode], 0U, mid);
    const HalfContent right =
        measure_half(frame, empty[mode], mid, frame.width);
    std::printf("instanced_batches_gpu_test: %s path %ux%u left %u px "
                "(R-G %+.1f) right %u px (R-G %+.1f)\n",
                label, frame.width, frame.height, left.changedPixels,
                left.redMinusGreen, right.changedPixels, right.redMinusGreen);

    // Three objects cover several thousand pixels from here, and a group
    // drawn with the other group's matrices leaves its own half untouched,
    // so 500 separates present from absent with room for another window
    // size.
    constexpr std::uint32_t kPresent = 500U;
    if ((left.changedPixels < kPresent) || (right.changedPixels < kPresent)) {
      std::fprintf(stderr, "FAIL (%s): a batch is missing from its own side "
                           "(left %u px, right %u px)\n",
                   label, left.changedPixels, right.changedPixels);
      result = deferred ? 20 : 30;
    }
    // The red cubes belong on the left and the green spheres on the right.
    if ((left.redMinusGreen <= 0.0) || (right.redMinusGreen >= 0.0)) {
      std::fprintf(stderr, "FAIL (%s): the wrong batch is on a side (left "
                           "R-G %+.1f, right R-G %+.1f)\n",
                   label, left.redMinusGreen, right.redMinusGreen);
      result = deferred ? 21 : 31;
    }
  }
  engine::tests::checked(engine::core::cvar_set_bool("r_deferred", true), "r_deferred");
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("instanced_batches_gpu_test", &run);
}
