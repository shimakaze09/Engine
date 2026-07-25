// Verifies thumbnail resampling at image boundaries and invalid inputs.

#include "../test_harness.h"

#include "thumbnail_resample.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using engine::tests::TestContext;

void test_horizontal_upscale_clamps_edges(TestContext &ctx) {
  constexpr std::array<std::uint8_t, 8> source = {
      0U, 10U, 20U, 255U, 100U, 110U, 120U, 128U};
  std::array<std::uint8_t, 16> output{};
  constexpr std::array<std::uint8_t, 16> expected = {
      0U,  10U,  20U,  255U, 25U,  35U,  45U,  223U,
      75U, 85U, 95U, 160U, 100U, 110U, 120U, 128U};

  ctx.check(engine::tools::resize_rgba_bilinear(
                source.data(), 2, 1, output.data(), 4, 1),
            "horizontal upscale succeeds");
  ctx.check(output == expected,
            "horizontal upscale clamps boundary samples exactly");
}

void test_vertical_upscale_clamps_edges(TestContext &ctx) {
  constexpr std::array<std::uint8_t, 8> source = {
      0U, 20U, 40U, 255U, 80U, 100U, 120U, 127U};
  std::array<std::uint8_t, 16> output{};
  constexpr std::array<std::uint8_t, 16> expected = {
      0U,  20U, 40U,  255U, 20U, 40U,  60U,  223U,
      60U, 80U, 100U, 159U, 80U, 100U, 120U, 127U};

  ctx.check(engine::tools::resize_rgba_bilinear(
                source.data(), 1, 2, output.data(), 1, 4),
            "vertical upscale succeeds");
  ctx.check(output == expected,
            "vertical upscale clamps boundary samples exactly");
}

void test_single_pixel_is_preserved(TestContext &ctx) {
  constexpr std::array<std::uint8_t, 4> source = {17U, 63U, 129U, 201U};
  std::array<std::uint8_t, 36> output{};
  std::array<std::uint8_t, 36> expected{};
  for (std::size_t i = 0U; i < expected.size(); i += 4U) {
    expected[i + 0U] = source[0];
    expected[i + 1U] = source[1];
    expected[i + 2U] = source[2];
    expected[i + 3U] = source[3];
  }

  ctx.check(engine::tools::resize_rgba_bilinear(
                source.data(), 1, 1, output.data(), 3, 3),
            "single pixel upscale succeeds");
  ctx.check(output == expected, "single pixel color is preserved exactly");
}

void test_invalid_arguments_fail(TestContext &ctx) {
  std::array<std::uint8_t, 4> pixels{};
  ctx.check(!engine::tools::resize_rgba_bilinear(
                nullptr, 1, 1, pixels.data(), 1, 1),
            "null source rejected");
  ctx.check(!engine::tools::resize_rgba_bilinear(
                pixels.data(), 0, 1, pixels.data(), 1, 1),
            "zero source width rejected");
  ctx.check(!engine::tools::resize_rgba_bilinear(
                pixels.data(), 1, 1, pixels.data(), 1, 0),
            "zero destination height rejected");
}

} // namespace

int main() {
  TestContext ctx{};
  test_horizontal_upscale_clamps_edges(ctx);
  test_vertical_upscale_clamps_edges(ctx);
  test_single_pixel_is_preserved(ctx);
  test_invalid_arguments_fail(ctx);
  return ctx.finish("thumbnail_resample_test");
}
