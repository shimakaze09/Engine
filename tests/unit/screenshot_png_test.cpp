// Verifies the screenshot PNG writer: a BGRA readback lands as an opaque RGB
// PNG with exactly its pixels, honouring the row pitch and a bottom-up
// readback; a region crops to its rectangle, clamped to the frame; a region
// off the frame, bad arguments and an unwritable destination each fail with
// one logged line and leave the file that was there untouched.

#include "../test_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"
#include "screenshot_png.h"

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

namespace rr = engine::renderer;

constexpr const char *kOutputPath = "screenshot_png_test_output.png";

std::size_t g_errors = 0U;

void count_errors(engine::core::LogLevel level, const char *channel,
                  const char *, void *) noexcept {
  if ((level == engine::core::LogLevel::Error) && (channel != nullptr) &&
      (std::strcmp(channel, "renderer") == 0)) {
    ++g_errors;
  }
}

/// A 3x2 BGRA readback, top row first, with 4 bytes of padding per row so
/// the writer must follow the pitch. Texel (x, y) is B=10x+y, G=100+x,
/// R=200+y, and its alpha is 0, which the PNG must not carry.
constexpr std::uint32_t kWidth = 3U;
constexpr std::uint32_t kHeight = 2U;
constexpr std::uint32_t kPitch = (kWidth * 4U) + 4U;

void fill_readback(std::uint8_t *data, bool bottomUp) noexcept {
  std::memset(data, 0xEE, static_cast<std::size_t>(kPitch) * kHeight);
  for (std::uint32_t y = 0U; y < kHeight; ++y) {
    const std::uint32_t row = bottomUp ? (kHeight - 1U - y) : y;
    for (std::uint32_t x = 0U; x < kWidth; ++x) {
      std::uint8_t *texel = data + (row * kPitch) + (x * 4U);
      texel[0] = static_cast<std::uint8_t>((10U * x) + y);
      texel[1] = static_cast<std::uint8_t>(100U + x);
      texel[2] = static_cast<std::uint8_t>(200U + y);
      texel[3] = 0U;
    }
  }
}

/// True when the PNG at kOutputPath is `width` x `height` RGB and texel
/// (x, y) of it is texel (x + dx, y + dy) of the readback.
bool png_matches(std::uint32_t width, std::uint32_t height, std::uint32_t dx,
                 std::uint32_t dy) noexcept {
  int w = 0;
  int h = 0;
  int channels = 0;
  unsigned char *pixels = stbi_load(kOutputPath, &w, &h, &channels, 0);
  if (pixels == nullptr) {
    return false;
  }
  bool same = (w == static_cast<int>(width)) &&
              (h == static_cast<int>(height)) && (channels == 3);
  for (std::uint32_t y = 0U; same && (y < height); ++y) {
    for (std::uint32_t x = 0U; same && (x < width); ++x) {
      const unsigned char *rgb = pixels + (((y * width) + x) * 3U);
      same = (rgb[0] == 200U + y + dy) && (rgb[1] == 100U + x + dx) &&
             (rgb[2] == (10U * (x + dx)) + y + dy);
    }
  }
  stbi_image_free(pixels);
  return same;
}

void remove_output() noexcept {
  std::error_code ec{};
  std::filesystem::remove(kOutputPath, ec);
}

void check_full_frame(engine::tests::TestContext &t) noexcept {
  std::uint8_t data[kPitch * kHeight] = {};
  fill_readback(data, false);
  g_errors = 0U;
  t.check(rr::write_bgra_png(kOutputPath, kWidth, kHeight, kPitch, data, false,
                             nullptr) &&
              png_matches(kWidth, kHeight, 0U, 0U) && (g_errors == 0U),
          "the whole frame lands as an opaque RGB PNG of its pixels");

  fill_readback(data, true);
  t.check(rr::write_bgra_png(kOutputPath, kWidth, kHeight, kPitch, data, true,
                             nullptr) &&
              png_matches(kWidth, kHeight, 0U, 0U),
          "a bottom-up readback lands the right way up");
}

void check_regions(engine::tests::TestContext &t) noexcept {
  std::uint8_t data[kPitch * kHeight] = {};
  fill_readback(data, true);
  const rr::ScreenshotRegion inner{1, 1, 2, 1};
  t.check(rr::write_bgra_png(kOutputPath, kWidth, kHeight, kPitch, data, true,
                             &inner) &&
              png_matches(2U, 1U, 1U, 1U),
          "a region keeps exactly its rectangle");

  const rr::ScreenshotRegion overhanging{2, -5, 10, 10};
  t.check(rr::write_bgra_png(kOutputPath, kWidth, kHeight, kPitch, data, true,
                             &overhanging) &&
              png_matches(1U, 2U, 2U, 0U),
          "a region partly off the frame is clamped to it");

  const rr::ScreenshotRegion outside{5, 5, 4, 4};
  g_errors = 0U;
  t.check(!rr::write_bgra_png(kOutputPath, kWidth, kHeight, kPitch, data, true,
                              &outside) &&
              (g_errors == 1U) && png_matches(1U, 2U, 2U, 0U),
          "a region off the frame fails, logged, the old file untouched");
}

void check_refusals(engine::tests::TestContext &t) noexcept {
  std::uint8_t data[kPitch * kHeight] = {};
  fill_readback(data, false);
  g_errors = 0U;
  t.check(!rr::write_bgra_png(nullptr, kWidth, kHeight, kPitch, data, false,
                              nullptr) &&
              !rr::write_bgra_png(kOutputPath, kWidth, kHeight, kPitch,
                                  nullptr, false, nullptr) &&
              !rr::write_bgra_png(kOutputPath, 0U, kHeight, kPitch, data,
                                  false, nullptr) &&
              !rr::write_bgra_png(kOutputPath, kWidth, kHeight, 4U, data,
                                  false, nullptr) &&
              (g_errors == 4U),
          "bad arguments fail, one logged line each");
  t.check(png_matches(1U, 2U, 2U, 0U),
          "and leave the file that was there untouched");

  g_errors = 0U;
  t.check(!rr::write_bgra_png("no_such_directory_for_screenshots/shot.png",
                              kWidth, kHeight, kPitch, data, false, nullptr) &&
              (g_errors >= 1U),
          "an unwritable destination fails, logged");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_errors, nullptr)) {
    return 1;
  }
  remove_output();

  engine::tests::TestContext t;
  check_full_frame(t);
  check_regions(t);
  check_refusals(t);

  engine::core::log_unregister_sink(&count_errors, nullptr);
  remove_output();
  return t.finish("screenshot_png");
}
