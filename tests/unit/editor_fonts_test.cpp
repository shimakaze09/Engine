// The editor's font chain (#610): Roboto for Latin text with a CJK face
// merged behind it, so a Chinese or Japanese entity name, folder or field
// renders instead of drawing as missing-glyph boxes. The face is the
// author's chosen file, else the Noto Sans SC that ships in the engine's
// content (engine/fonts), else a system font; so the glyph checks run on
// every machine, fonts or not. The chosen file persists as a preference in the
// layout file.

#include "editor_fonts.h"
#include "editor_preferences.h"

#include "../test_harness.h"

#include "../asset_root.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

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

} // namespace

/// Runs this executable or test program.
int main() {
  TestContext t;
  if (!engine::tests::enter_asset_root()) {
    t.fail("locate the repository's engine_assets/fonts");
    return t.finish("editor_fonts");
  }
  // The engine mount as bootstrap makes it: the bundled faces load through
  // it.
  const std::string engineRoot = engine::tests::engine_root_path();
  const std::string roboto = engineRoot + "/fonts/Roboto-Medium.ttf";
  t.check(engine::core::mount("engine", engineRoot.c_str()),
          "mount the engine content");
  t.check(engine::core::initialize_logging(), "initialize logging");
  t.check(engine::core::log_register_sink(&count_override_warnings, nullptr),
          "register the warning sink");

  // --- No override: the bundled face, on any machine.
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
  t.check(system.cjk && (std::strcmp(system.cjkPath,
                                     engine::editor::kBundledCjkFontPath) == 0),
          "the bundled CJK face is merged when no font is chosen");
  t.check(glyphsResolve,
          "Chinese and Japanese characters resolve to glyphs in the editor "
          "font");

  // --- An override that cannot be read is reported and does not stop the
  // chain.
  g_overrideWarnings = 0;
  const EditorFontResult missing =
      with_fonts("no/such/font.ttc", [](ImFont *) {});
  t.check(g_overrideWarnings == 1,
          "an unreadable editor.cjk_font is reported once");
  t.check((missing.cjk == system.cjk) &&
              (std::strcmp(missing.cjkPath, system.cjkPath) == 0),
          "and the bundled face still loads");

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
  const EditorFontResult chosen = with_fonts(roboto.c_str(), [](ImFont *) {});
  t.check(chosen.cjk && (std::strcmp(chosen.cjkPath, roboto.c_str()) == 0),
          "editor.cjk_font is tried before the bundled and system fonts");

  // --- The chosen file persists with the layout: the preferences section
  // written into the layout file carries it, and loading that section
  // restores it before the fonts are built.
  {
    ImGuiContext *context = ImGui::CreateContext();
    t.check(engine::core::initialize_cvars(), "initialize cvars");
    engine::editor::register_editor_preferences();
    t.check(
        engine::core::cvar_set_string("editor.cjk_font", "fonts/custom.ttf"),
        "choose a font");
    char section[256] = {};
    const std::size_t written =
        engine::editor::editor_preferences_section(section, sizeof(section));
    t.check(
        (written > 0U) &&
            (std::strstr(section, "[EnginePreferences][Editor]") != nullptr) &&
            (std::strstr(section, "CjkFont=fonts/custom.ttf") != nullptr),
        "the layout file carries the chosen font");
    t.check(engine::core::cvar_set_string("editor.cjk_font", ""),
            "forget the choice");
    ImGui::LoadIniSettingsFromMemory(section, written);
    t.check(std::strcmp(engine::core::cvar_get_string("editor.cjk_font", ""),
                        "fonts/custom.ttf") == 0,
            "loading the layout restores it");
    engine::core::shutdown_cvars();
    ImGui::DestroyContext(context);
  }

  engine::core::shutdown_logging();
  return t.finish("editor_fonts");
}
