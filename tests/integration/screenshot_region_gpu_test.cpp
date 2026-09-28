// Verifies on a real render device that a screenshot request saves the
// frame it asks for (#767 F3): a lit scene is captured whole through the
// diagnostic readback, then a rectangle of the next, identical frame is
// requested as a PNG through renderer::request_screenshot. The PNG's pixels
// must equal that rectangle of the whole frame exactly (PNG is lossless and
// the scene does not change), and the request must report success with its
// path. A second request while the first is still waiting is refused.

#include "../gpu_scene_fixture.h"

#include "engine/renderer/screenshot.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4505)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr const char *kRegionPath = "screenshot_region_gpu_test.png";

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  engine::tests::checked(engine::core::cvar_set_bool("r_bloom", false),
                         "r_bloom");
  const Entity cube = engine::tests::add_builtin_mesh(
      world, "builtin://cube", engine::runtime::Transform{},
      engine::math::Vec3(0.9F, 0.3F, 0.2F));
  if ((cube == kInvalidEntity) ||
      !engine::tests::look_from(world, engine::math::Vec3(2.0F, 1.5F, 3.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  CapturedFrame whole{};
  if (!engine::tests::settle_frames(pipeline, 20) ||
      !engine::tests::capture_presented_frame(
          pipeline, "screenshot_region_gpu_whole.tga", &whole)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  // A rectangle through the middle of the frame, where the cube is.
  engine::renderer::ScreenshotRegion region{};
  region.x = static_cast<std::int32_t>(whole.width / 4U);
  region.y = static_cast<std::int32_t>(whole.height / 3U);
  region.width = static_cast<std::int32_t>(whole.width / 2U);
  region.height = static_cast<std::int32_t>(whole.height / 3U);

  std::error_code ec{};
  std::filesystem::remove(kRegionPath, ec);
  const std::uint32_t before =
      engine::renderer::last_screenshot_result().sequence;
  if (!engine::renderer::request_screenshot(kRegionPath, &region)) {
    std::fprintf(stderr, "FAIL: the request was refused\n");
    return 11;
  }
  if (engine::renderer::request_screenshot("second.png", nullptr)) {
    std::fprintf(stderr, "FAIL: a second request was taken while the first "
                         "waited\n");
    return 12;
  }
  engine::renderer::ScreenshotResult result{};
  for (int frame = 0; frame < 16; ++frame) {
    if (!pipeline.execute_frame()) {
      return 13;
    }
    result = engine::renderer::last_screenshot_result();
    if (result.sequence != before) {
      break;
    }
  }
  if ((result.sequence != before + 1U) || !result.succeeded ||
      (std::strcmp(result.path, kRegionPath) != 0)) {
    std::fprintf(stderr, "FAIL: the request did not report its saved file\n");
    return 14;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  unsigned char *pixels = stbi_load(kRegionPath, &width, &height, &channels, 0);
  if ((pixels == nullptr) || (width != region.width) ||
      (height != region.height) || (channels != 3)) {
    std::fprintf(stderr, "FAIL: the PNG is not the region's size\n");
    stbi_image_free(pixels);
    return 15;
  }
  std::size_t mismatches = 0U;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const unsigned char *rgb =
          pixels + ((static_cast<std::size_t>(y) * static_cast<std::size_t>(
                                                       width)) +
                    static_cast<std::size_t>(x)) *
                       3U;
      const auto wx = static_cast<std::uint32_t>(region.x + x);
      const auto wy = static_cast<std::uint32_t>(region.y + y);
      if ((rgb[0] != whole.channel(wx, wy, 2U)) ||
          (rgb[1] != whole.channel(wx, wy, 1U)) ||
          (rgb[2] != whole.channel(wx, wy, 0U))) {
        ++mismatches;
      }
    }
  }
  stbi_image_free(pixels);
  std::filesystem::remove(kRegionPath, ec);
  if (mismatches != 0U) {
    std::fprintf(stderr,
                 "FAIL: %zu of the region's pixels differ from the frame\n",
                 mismatches);
    return 16;
  }
  return 0;
}

} // namespace

int main() {
  return engine::tests::run_gpu_scene_test("screenshot_region_gpu_test", &run);
}
