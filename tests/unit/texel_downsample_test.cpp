// Verifies the CPU downsample that fills a texture's mip chain (issue
// #709): each level is the exact box average of the level above, for 8-bit
// and float texels, with odd extents folding their last row or column into
// the final destination texel and 1-texel extents kept; bad arguments are
// refused with nothing written. An sRGB texture's colour components are
// averaged in linear light and encoded back (issue #810), its alpha as is.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "texel_downsample.h"

#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests{};

using engine::renderer::downsample_texels;
using engine::renderer::linear_to_srgb_byte;
using engine::renderer::mip_extent;
using engine::renderer::srgb_byte_to_linear;
using engine::renderer::TexelData;

void check_extents() {
  g_tests.check(mip_extent(256, 0) == 256, "level 0 is the image");
  g_tests.check(mip_extent(256, 4) == 16, "each level halves");
  g_tests.check(mip_extent(5, 1) == 2, "an odd extent rounds down");
  g_tests.check(mip_extent(1, 3) == 1, "never below one texel");
  g_tests.check(mip_extent(256, 20) == 1, "past the chain stays one texel");
}

void check_u8() {
  // 2x2, one component: (0 + 1 + 2 + 3) / 4 = 1.5 rounds half up to 2.
  const std::uint8_t quad[4] = {0U, 1U, 2U, 3U};
  std::uint8_t one = 0xAAU;
  g_tests.check(downsample_texels(TexelData::U8, 1, quad, 2, 2, &one) &&
                    (one == 2U),
                "a 2x2 block averages, rounding half up");

  // 4x2 RGBA to 2x1: each channel averages its own four texels.
  const std::uint8_t rgba[4 * 2 * 4] = {
      10, 20, 30, 40, 12, 22, 32, 42, 100, 0, 255, 1, 100, 0, 255, 1,
      14, 24, 34, 44, 16, 26, 36, 46, 100, 0, 255, 3, 100, 0, 255, 3};
  std::uint8_t out[2 * 4] = {};
  const std::uint8_t expected[2 * 4] = {13, 23, 33, 43, 100, 0, 255, 2};
  g_tests.check(downsample_texels(TexelData::U8, 4, rgba, 4, 2, out) &&
                    (std::memcmp(out, expected, sizeof(out)) == 0),
                "RGBA channels average independently");

  // 5x1 to 2x1: the odd tail folds into the last texel.
  const std::uint8_t row[5] = {0U, 10U, 20U, 30U, 40U};
  std::uint8_t pair[2] = {};
  g_tests.check(downsample_texels(TexelData::U8, 1, row, 5, 1, pair) &&
                    (pair[0] == 5U) && (pair[1] == 30U),
                "an odd width's last texel takes three source texels");

  // 1x4 to 1x2: a 1-texel width stays one texel.
  const std::uint8_t column[4] = {0U, 2U, 4U, 8U};
  std::uint8_t halves[2] = {};
  g_tests.check(downsample_texels(TexelData::U8, 1, column, 1, 4, halves) &&
                    (halves[0] == 1U) && (halves[1] == 6U),
                "a 1-texel width averages only down its column");

  // A saturated block cannot overflow the sum.
  const std::uint8_t white[3 * 3] = {255, 255, 255, 255, 255,
                                     255, 255, 255, 255};
  std::uint8_t whiteOut = 0U;
  g_tests.check(downsample_texels(TexelData::U8, 1, white, 3, 3, &whiteOut) &&
                    (whiteOut == 255U),
                "a white 3x3 block stays white");
}

void check_f32() {
  // 3x3 RGB to 1x1: the one destination texel is the mean of all nine,
  // exactly (the sums are integers well inside float precision).
  float image[3 * 3 * 3] = {};
  for (int i = 0; i < 9; ++i) {
    image[i * 3 + 0] = static_cast<float>(i);
    image[i * 3 + 1] = static_cast<float>(i * 2);
    image[i * 3 + 2] = 1.0F;
  }
  float out[3] = {};
  g_tests.check(downsample_texels(TexelData::F32, 3, image, 3, 3, out) &&
                    (out[0] == 4.0F) && (out[1] == 8.0F) && (out[2] == 1.0F),
                "a 3x3 float block averages all nine texels");

  // HDR values above 1 are kept, not clamped.
  const float bright[2 * 2] = {8.0F, 16.0F, 32.0F, 64.0F};
  float brightOut = 0.0F;
  g_tests.check(
      downsample_texels(TexelData::F32, 1, bright, 2, 2, &brightOut) &&
          (brightOut == 30.0F),
      "HDR values average unclamped");
}

void check_srgb() {
  // IEC 61966-2-1: code 128 is linear 0.2158605, code 188 is 0.5029.
  g_tests.check(std::fabs(srgb_byte_to_linear(128U) - 0.2158605F) < 1.0e-6F,
                "sRGB code 128 decodes to linear 0.2158605");
  g_tests.check((srgb_byte_to_linear(0U) == 0.0F) &&
                    (srgb_byte_to_linear(255U) == 1.0F),
                "black and white decode exactly");
  bool roundTrips = true;
  for (int code = 0; code < 256; ++code) {
    const auto byte = static_cast<std::uint8_t>(code);
    roundTrips =
        roundTrips && (linear_to_srgb_byte(srgb_byte_to_linear(byte)) == byte);
  }
  g_tests.check(roundTrips, "every sRGB code encodes back to itself");

  // A black-and-white checker averages to half the light, code 188, not
  // to the byte average 128, which is a fifth of it and reads dark.
  const std::uint8_t checker[2 * 2 * 4] = {
      0, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 255};
  std::uint8_t mean[4] = {};
  g_tests.check(
      downsample_texels(TexelData::U8, 4, checker, 2, 2, mean, true) &&
          (mean[0] == 188U) && (mean[1] == 188U) && (mean[2] == 188U),
      "sRGB colour averages in linear light");
  g_tests.check(mean[3] == 128U, "sRGB alpha averages as linear bytes");
  std::uint8_t plain[4] = {};
  g_tests.check(downsample_texels(TexelData::U8, 4, checker, 2, 2, plain) &&
                    (plain[0] == 128U),
                "a linear texture still averages its bytes");
}

void check_refusals() {
  const std::uint8_t src[4] = {1U, 2U, 3U, 4U};
  std::uint8_t dst = 0x5AU;
  g_tests.check(!downsample_texels(TexelData::U8, 1, nullptr, 2, 2, &dst) &&
                    !downsample_texels(TexelData::U8, 1, src, 2, 2, nullptr) &&
                    !downsample_texels(TexelData::U8, 1, src, 0, 2, &dst) &&
                    !downsample_texels(TexelData::U8, 1, src, 2, -1, &dst) &&
                    !downsample_texels(TexelData::U8, 0, src, 2, 2, &dst) &&
                    !downsample_texels(TexelData::U8, 5, src, 2, 2, &dst) &&
                    (dst == 0x5AU),
                "bad arguments are refused with nothing written");
}

} // namespace

int main() {
  check_extents();
  check_u8();
  check_f32();
  check_srgb();
  check_refusals();
  return g_tests.finish("texel downsample tests");
}
