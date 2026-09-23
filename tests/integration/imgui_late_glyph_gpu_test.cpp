// GPU regression for issue #686: a glyph ImGui rasterizes after its font
// atlas already exists must reach the texture the editor draws text with.
//
// ImGui 1.92 bakes glyphs on demand. The first frame that draws asks the
// renderer to create the atlas; every glyph needed later arrives as a
// rectangle update to that texture. bgfx makes a texture created with
// initial memory immutable and drops every update to it with only a debug
// warning, so the editor drew each late glyph from an unwritten region:
// on real hardware the Console lost 'j', '=', '6' and '9' mid-line while
// the glyphs its first frame had drawn stayed intact.
//
// The test boots the real device, drives the editor's own ImGui bgfx
// renderer with one glyph while the atlas is created, then adds a second
// glyph once the atlas exists, and reads back the presented frame. It
// presents directly rather than through pipeline frames, so the back
// buffer holds the overlay and nothing else. The early glyph is the
// positive control: a frame the overlay never reached must fail rather
// than pass.

#include "../gpu_scene_fixture.h"

#include "engine/core/platform.h"

#include "imgui_impl_bgfx.h"

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace {

using engine::tests::CapturedFrame;

/// Large enough that a glyph covers hundreds of pixels, so the counts
/// below are far from any threshold.
constexpr float kGlyphSize = 96.0F;
/// Glyph origins, in pixels from the top-left of the back buffer.
constexpr float kEarlyX = 32.0F;
constexpr float kLateX = 160.0F;
constexpr float kGlyphY = 32.0F;
/// The box a glyph is counted in: its origin plus the glyph size.
constexpr std::uint32_t kBox = static_cast<std::uint32_t>(kGlyphSize);
/// Text is white on a black panel; a texel counts as lit above this.
constexpr std::uint8_t kLitLevel = 128U;
/// Presents before a capture: the atlas's creation and any resize that
/// follows the first bake have settled well before this.
constexpr int kSettlePresents = 8;
/// Presents a capture may take to land: bgfx returns the pixels a frame or
/// two after the request.
constexpr int kMaxCapturePresents = 16;

/// Draws one overlay frame, a black panel with the early glyph and, when
/// asked, the late one, through the editor's renderer, and presents it.
void present_overlay(bool drawLateGlyph) noexcept {
  int width = 0;
  int height = 0;
  engine::core::render_drawable_size(&width, &height);
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize =
      ImVec2(static_cast<float>(width), static_cast<float>(height));
  io.DeltaTime = 1.0F / 60.0F;
  ImGui::NewFrame();
  ImDrawList *drawList = ImGui::GetForegroundDrawList();
  drawList->AddRectFilled(ImVec2(0.0F, 0.0F),
                          ImVec2(kLateX + 2.0F * kGlyphSize,
                                 kGlyphY + 2.0F * kGlyphSize),
                          IM_COL32(0, 0, 0, 255));
  drawList->AddText(nullptr, kGlyphSize, ImVec2(kEarlyX, kGlyphY),
                    IM_COL32_WHITE, "H");
  if (drawLateGlyph) {
    drawList->AddText(nullptr, kGlyphSize, ImVec2(kLateX, kGlyphY),
                      IM_COL32_WHITE, "W");
  }
  ImGui::Render();
  ImGui_ImplBgfx_RenderDrawData(ImGui::GetDrawData());
  engine::renderer::present_render_device();
}

/// Settles the overlay, then captures the frame it presents.
bool capture_overlay(bool drawLateGlyph, const char *path,
                     CapturedFrame *out) noexcept {
  for (int present = 0; present < kSettlePresents; ++present) {
    present_overlay(drawLateGlyph);
  }
  std::error_code ec{};
  std::filesystem::remove(path, ec);
  if (!engine::renderer::render_device_bgfx_request_screenshot(path)) {
    return false;
  }
  for (int present = 0; present < kMaxCapturePresents; ++present) {
    present_overlay(drawLateGlyph);
    if (std::filesystem::exists(path, ec) &&
        engine::tests::load_captured_tga(path, out)) {
      return true;
    }
  }
  return false;
}

/// Counts the texels in a glyph's box whose every colour channel is lit.
std::uint32_t lit_texels(const CapturedFrame &frame, float originX) noexcept {
  const auto x0 = static_cast<std::uint32_t>(originX);
  const auto y0 = static_cast<std::uint32_t>(kGlyphY);
  std::uint32_t lit = 0U;
  for (std::uint32_t y = y0; (y < y0 + kBox) && (y < frame.height); ++y) {
    for (std::uint32_t x = x0; (x < x0 + kBox) && (x < frame.width); ++x) {
      if ((frame.channel(x, y, 0U) > kLitLevel) &&
          (frame.channel(x, y, 1U) > kLitLevel) &&
          (frame.channel(x, y, 2U) > kLitLevel)) {
        ++lit;
      }
    }
  }
  return lit;
}

int run(engine::EnginePipeline &, engine::runtime::World &) noexcept {
  ImGui::CreateContext();
  ImGui::GetIO().Fonts->AddFontDefault();
  if (!ImGui_ImplBgfx_Init()) {
    ImGui::DestroyContext();
    return 10;
  }
  CapturedFrame early{};
  CapturedFrame late{};
  const bool earlyCaptured =
      capture_overlay(false, "imgui_late_glyph_early.tga", &early);
  const bool lateCaptured =
      earlyCaptured && capture_overlay(true, "imgui_late_glyph_late.tga", &late);
  ImGui_ImplBgfx_Shutdown();
  ImGui::DestroyContext();

  if (!earlyCaptured) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  if (!lateCaptured) {
    return 11;
  }
  const std::uint32_t earlyBefore = lit_texels(early, kEarlyX);
  const std::uint32_t lateBefore = lit_texels(early, kLateX);
  const std::uint32_t earlyAfter = lit_texels(late, kEarlyX);
  const std::uint32_t lateAfter = lit_texels(late, kLateX);
  std::printf("imgui_late_glyph_gpu_test: lit texels, early glyph %u then "
              "%u; late glyph's box %u before it is drawn, %u after\n",
              earlyBefore, earlyAfter, lateBefore, lateAfter);

  int result = 0;
  // The positive control: the overlay reached the frame at all.
  if (earlyBefore == 0U) {
    std::fprintf(stderr, "FAIL: the early glyph never drew\n");
    result = 12;
  }
  // The box is empty until the glyph is drawn, so what is lit after is
  // the glyph and nothing else.
  if (lateBefore != 0U) {
    std::fprintf(stderr, "FAIL: the late glyph's box was lit before it was "
                         "drawn\n");
    result = 13;
  }
  // The defect: the late glyph's texels were never uploaded.
  if (lateAfter == 0U) {
    std::fprintf(stderr, "FAIL: a glyph rasterized after the atlas was "
                         "created drew nothing\n");
    result = 14;
  }
  // Updating the atlas must not disturb a glyph already in it.
  if (earlyAfter != earlyBefore) {
    std::fprintf(stderr, "FAIL: the early glyph changed when the late one "
                         "was added\n");
    result = 15;
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("imgui_late_glyph_gpu_test", &run);
}
