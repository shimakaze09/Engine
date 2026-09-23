// The editor's font chain (#610): Roboto for Latin text with a CJK face
// merged behind it, so a Chinese or Japanese entity name, folder or field
// renders instead of drawing as missing-glyph boxes. Before, the editor
// loaded Roboto alone and every CJK character fell through to the box.
//
// No CJK face ships with the engine; the chain uses the system's. On a
// machine with none of the candidates the glyph checks cannot run and are
// reported as skipped rather than passed.

#include "editor_fonts.h"

#include "../test_harness.h"

#include "engine/core/logging.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

using engine::editor::EditorFontResult;
using engine::editor::load_editor_fonts;
using engine::tests::TestContext;

int g_overrideWarnings = 0;

void count_override_warnings(engine::core::LogLevel level, const char *channel,
                             const char *message,
                             void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (channel != nullptr) &&
      (std::strcmp(channel, "editor") == 0) && (message != nullptr) &&
      (std::strstr(message, "editor.cjk_font") != nullptr)) {
    ++g_overrideWarnings;
  }
}

/// Runs the chain in a fresh ImGui context whose renderer honours texture
/// requests, as the editor's does; `probe` then sees the loaded font.
template <typename Probe>
EditorFontResult with_fonts(const char *cjkOverride, Probe &&probe) {
  ImGuiContext *context = ImGui::CreateContext();
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  const EditorFontResult result =
      load_editor_fonts(ImGui::GetIO().Fonts, 17.0F, cjkOverride);
  ImFont *font = (ImGui::GetIO().Fonts->Fonts.Size > 0)
                     ? ImGui::GetIO().Fonts->Fonts[0]
                     : nullptr;
  probe(font);
  ImGui::DestroyContext(context);
  return result;
}

bool find_repository_root() {
  std::error_code ec{};
  std::filesystem::path dir = std::filesystem::current_path(ec);
  for (int i = 0; (i < 6) && !ec; ++i) {
    if (std::filesystem::exists(dir / "assets/fonts/Roboto-Medium.ttf", ec)) {
      std::filesystem::current_path(dir, ec);
      return !ec;
    }
    dir = dir.parent_path();
  }
  return false;
}

} // namespace

/// Runs this executable or test program.
int main() {
  TestContext t;
  if (!find_repository_root()) {
    t.fail("locate the repository's assets/fonts");
    return t.finish("editor_fonts");
  }
  t.check(engine::core::initialize_logging(), "initialize logging");
  t.check(engine::core::log_register_sink(&count_override_warnings, nullptr),
          "register the warning sink");

  // --- The system chain, no override.
  bool glyphsResolve = false;
  const EditorFontResult system =
      with_fonts("", [&glyphsResolve](ImFont *font) {
        // 主 and 角 (Chinese), の and ア (Japanese kana).
        glyphsResolve = (font != nullptr) && font->IsGlyphInFont(0x4E3B) &&
                        font->IsGlyphInFont(0x89D2) &&
                        font->IsGlyphInFont(0x306E) &&
                        font->IsGlyphInFont(0x30A2);
      });
  t.check(system.latin, "Roboto loads for Latin text");
  // Whether this machine has a candidate is decided apart from the chain,
  // so a chain that stopped merging fails here instead of looking like a
  // machine without fonts.
  bool candidatePresent = false;
  std::size_t candidateCount = 0U;
  const char *const *candidates =
      engine::editor::editor_cjk_font_candidates(&candidateCount);
  for (std::size_t i = 0U; i < candidateCount; ++i) {
    std::error_code ec{};
    candidatePresent =
        candidatePresent || std::filesystem::exists(candidates[i], ec);
  }
  if (candidatePresent) {
    t.check(system.cjk, "a system CJK font present is merged");
    t.check(glyphsResolve,
            "Chinese and Japanese characters resolve to glyphs in the editor "
            "font");
  } else {
    t.skip("no system CJK font on this machine; set editor.cjk_font to "
           "check the glyphs");
  }

  // --- An override that cannot be read is reported and does not stop the
  // system chain.
  g_overrideWarnings = 0;
  const EditorFontResult missing =
      with_fonts("no/such/font.ttc", [](ImFont *) {});
  t.check(g_overrideWarnings == 1,
          "an unreadable editor.cjk_font is reported once");
  t.check((missing.cjk == system.cjk) &&
              (std::strcmp(missing.cjkPath, system.cjkPath) == 0),
          "and the system chain still runs");

  // --- An override too small to be a font is refused before ImGui sees
  // it: ImGui asserts on one, which would abort an assert-enabled editor.
  {
    std::FILE *file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, "editor_fonts_test_tiny.ttf", "wb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen("editor_fonts_test_tiny.ttf", "wb");
#endif
    const bool written =
        (file != nullptr) && (std::fwrite("not a font", 1U, 10U, file) == 10U);
    if (file != nullptr) {
      std::fclose(file);
    }
    t.check(written, "write a ten-byte file");
    g_overrideWarnings = 0;
    const EditorFontResult tiny =
        with_fonts("editor_fonts_test_tiny.ttf", [](ImFont *) {});
    std::remove("editor_fonts_test_tiny.ttf");
    t.check((g_overrideWarnings == 1) && (tiny.cjk == system.cjk),
            "a file too small to be a font is refused and reported");
  }

  // --- A readable override comes first. Any font file proves the order.
  const EditorFontResult chosen =
      with_fonts("assets/fonts/Roboto-Medium.ttf", [](ImFont *) {});
  t.check(chosen.cjk && (std::strcmp(chosen.cjkPath,
                                     "assets/fonts/Roboto-Medium.ttf") == 0),
          "editor.cjk_font is tried before the system fonts");

  engine::core::shutdown_logging();
  return t.finish("editor_fonts");
}
