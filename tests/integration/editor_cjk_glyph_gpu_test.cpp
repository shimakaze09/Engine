// GPU check for issue #610: Chinese and Japanese text in the editor draws
// as real glyphs, through the editor's own fonts and ImGui renderer on a
// real device.
//
// The test loads the editor's fonts exactly as the editor does (Roboto
// with the bundled CJK face merged behind it), draws three CJK glyphs and
// one codepoint no font has onto a black panel, presents through the
// editor's bgfx renderer and reads the frame back. A glyph the fonts lack
// draws ImGui's fallback, so every missing glyph looks the same: each CJK
// glyph must light its box, and differ from the others and from the
// fallback. A Latin glyph drawn beside them is the positive control.

#include "../gpu_scene_fixture.h"

#include "engine/core/platform.h"

#include "editor_fonts.h"
#include "imgui_impl_bgfx.h"

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace {

using engine::tests::CapturedFrame;

/// Large enough that a glyph covers hundreds of pixels.
constexpr float kGlyphSize = 64.0F;
constexpr float kGlyphY = 32.0F;
constexpr std::uint32_t kBox = static_cast<std::uint32_t>(kGlyphSize);
/// One glyph every kStride pixels, from x = kStride / 2.
constexpr float kStride = 96.0F;
/// Text is white on a black panel; a texel counts as lit above this.
constexpr std::uint8_t kLitLevel = 128U;
/// Texels two glyph masks must differ by to count as different glyphs: a
/// glyph covers hundreds, so this is far below any real difference and
/// far above sub-pixel noise between two draws of one glyph.
constexpr std::uint32_t kMinDifferentTexels = 40U;
constexpr int kSettlePresents = 8;
constexpr int kMaxCapturePresents = 16;

/// The glyphs drawn, left to right: a Latin control, three CJK glyphs (the
/// issue's 主 and 角, and the kana の), and a private-use codepoint no
/// font maps, which draws as the fallback.
constexpr const char *kGlyphs[] = {"H", "\xE4\xB8\xBB", "\xE8\xA7\x92",
                                   "\xE3\x81\xAE", "\xEE\x80\x80"};
constexpr std::size_t kGlyphCount = sizeof(kGlyphs) / sizeof(kGlyphs[0]);
constexpr std::size_t kFallback = kGlyphCount - 1U;

float glyph_x(std::size_t index) noexcept {
  return (kStride * 0.5F) + (kStride * static_cast<float>(index));
}

void present_overlay() noexcept {
  int width = 0;
  int height = 0;
  engine::core::render_drawable_size(&width, &height);
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize =
      ImVec2(static_cast<float>(width), static_cast<float>(height));
  io.DeltaTime = 1.0F / 60.0F;
  ImGui::NewFrame();
  ImDrawList *drawList = ImGui::GetForegroundDrawList();
  drawList->AddRectFilled(
      ImVec2(0.0F, 0.0F),
      ImVec2(glyph_x(kGlyphCount), kGlyphY + 2.0F * kGlyphSize),
      IM_COL32(0, 0, 0, 255));
  for (std::size_t i = 0U; i < kGlyphCount; ++i) {
    drawList->AddText(nullptr, kGlyphSize, ImVec2(glyph_x(i), kGlyphY),
                      IM_COL32_WHITE, kGlyphs[i]);
  }
  ImGui::Render();
  ImGui_ImplBgfx_RenderDrawData(ImGui::GetDrawData());
  engine::renderer::present_render_device();
}

bool capture_overlay(const char *path, CapturedFrame *out) noexcept {
  for (int present = 0; present < kSettlePresents; ++present) {
    present_overlay();
  }
  std::error_code ec{};
  std::filesystem::remove(path, ec);
  if (!engine::renderer::render_device_bgfx_request_screenshot(path)) {
    return false;
  }
  for (int present = 0; present < kMaxCapturePresents; ++present) {
    present_overlay();
    if (std::filesystem::exists(path, ec) &&
        engine::tests::load_captured_tga(path, out)) {
      return true;
    }
  }
  return false;
}

bool lit(const CapturedFrame &frame, std::uint32_t x,
         std::uint32_t y) noexcept {
  return (x < frame.width) && (y < frame.height) &&
         (frame.channel(x, y, 0U) > kLitLevel) &&
         (frame.channel(x, y, 1U) > kLitLevel) &&
         (frame.channel(x, y, 2U) > kLitLevel);
}

/// Lit texels in glyph `index`'s box.
std::uint32_t lit_texels(const CapturedFrame &frame,
                         std::size_t index) noexcept {
  const auto x0 = static_cast<std::uint32_t>(glyph_x(index));
  const auto y0 = static_cast<std::uint32_t>(kGlyphY);
  std::uint32_t count = 0U;
  for (std::uint32_t y = y0; y < y0 + kBox; ++y) {
    for (std::uint32_t x = x0; x < x0 + kBox; ++x) {
      count += lit(frame, x, y) ? 1U : 0U;
    }
  }
  return count;
}

/// Texels lit in exactly one of two glyph boxes, laid over each other.
std::uint32_t mask_difference(const CapturedFrame &frame, std::size_t a,
                              std::size_t b) noexcept {
  const auto ax = static_cast<std::uint32_t>(glyph_x(a));
  const auto bx = static_cast<std::uint32_t>(glyph_x(b));
  const auto y0 = static_cast<std::uint32_t>(kGlyphY);
  std::uint32_t count = 0U;
  for (std::uint32_t y = y0; y < y0 + kBox; ++y) {
    for (std::uint32_t dx = 0U; dx < kBox; ++dx) {
      count += (lit(frame, ax + dx, y) != lit(frame, bx + dx, y)) ? 1U : 0U;
    }
  }
  return count;
}

int run(engine::EnginePipeline &, engine::runtime::World &) noexcept {
  ImGui::CreateContext();
  const engine::editor::EditorFontResult fonts =
      engine::editor::load_editor_fonts(ImGui::GetIO().Fonts, kGlyphSize, "");
  if (!ImGui_ImplBgfx_Init()) {
    ImGui::DestroyContext();
    return 10;
  }
  CapturedFrame frame{};
  const bool captured = capture_overlay("editor_cjk_glyph.tga", &frame);
  ImGui_ImplBgfx_Shutdown();
  ImGui::DestroyContext();

  int result = 0;
  if (!fonts.latin || !fonts.cjk ||
      (std::strcmp(fonts.cjkPath, engine::editor::kBundledCjkFontPath) != 0)) {
    std::fprintf(stderr,
                 "FAIL: the editor fonts did not load the bundled "
                 "CJK face (latin %d, cjk %d, '%s')\n",
                 fonts.latin ? 1 : 0, fonts.cjk ? 1 : 0, fonts.cjkPath);
    result = 11;
  }
  if (!captured) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return result;
  }
  std::uint32_t litCounts[kGlyphCount] = {};
  for (std::size_t i = 0U; i < kGlyphCount; ++i) {
    litCounts[i] = lit_texels(frame, i);
  }
  std::printf("editor_cjk_glyph_gpu_test: lit texels H %u, 主 %u, 角 %u, "
              "の %u, fallback %u\n",
              litCounts[0], litCounts[1], litCounts[2], litCounts[3],
              litCounts[4]);

  // The positive control: the overlay reached the frame at all.
  if (litCounts[0] == 0U) {
    std::fprintf(stderr, "FAIL: the Latin control never drew\n");
    return 12;
  }
  for (std::size_t i = 1U; i < kFallback; ++i) {
    if (litCounts[i] == 0U) {
      std::fprintf(stderr, "FAIL: CJK glyph %zu drew nothing\n", i);
      result = 13;
    }
    const std::uint32_t fromFallback = mask_difference(frame, i, kFallback);
    if (fromFallback < kMinDifferentTexels) {
      std::fprintf(stderr,
                   "FAIL: CJK glyph %zu drew as the missing-glyph fallback "
                   "(%u texels differ)\n",
                   i, fromFallback);
      result = 14;
    }
    for (std::size_t j = i + 1U; j < kFallback; ++j) {
      const std::uint32_t apart = mask_difference(frame, i, j);
      if (apart < kMinDifferentTexels) {
        std::fprintf(stderr,
                     "FAIL: CJK glyphs %zu and %zu drew alike (%u texels "
                     "differ)\n",
                     i, j, apart);
        result = 15;
      }
    }
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("editor_cjk_glyph_gpu_test", &run);
}
