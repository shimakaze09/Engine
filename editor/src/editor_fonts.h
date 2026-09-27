// Loads the editor's UI fonts: Roboto for Latin text, with a CJK-capable
// face merged behind it so Chinese and Japanese names, folders and field
// text render instead of drawing as missing-glyph boxes.
//
// The CJK face is, in order: the file the author chose (`editor.cjk_font`,
// set from Window > Editor Settings), the Noto Sans SC face that ships in
// assets/fonts (Simplified and Traditional Chinese and Japanese kana, SIL
// OFL), and the operating system's own (Microsoft YaHei on Windows,
// PingFang or Hiragino on macOS, Noto CJK or WenQuanYi on Linux) should the
// bundled file be missing.

#pragma once

#include <cstddef>

struct ImFontAtlas;

namespace engine::editor {

/// The CJK face that ships with the engine, tried after the author's own.
inline constexpr const char *kBundledCjkFontPath =
    "assets/fonts/NotoSansSC-Medium.ttf";

/// The system font files tried for CJK glyphs, in order, for this platform.
const char *const *editor_cjk_font_candidates(std::size_t *outCount) noexcept;

/// What load_editor_fonts managed to load.
struct EditorFontResult final {
  /// Roboto loaded; false leaves ImGui's built-in font in use.
  bool latin = false;
  /// A CJK face was merged; the path it came from, or empty.
  bool cjk = false;
  char cjkPath[512] = {};
};

/// Adds the editor fonts to `atlas` at `sizePixels`. `cjkOverride`, when
/// non-empty, is tried before the bundled face and the system candidates. Needs
/// a renderer that honours ImGui's texture requests, since glyphs are
/// rasterized as they are first drawn rather than baked from fixed ranges. Logs
/// what it could not load; never fails the editor over a font.
EditorFontResult load_editor_fonts(ImFontAtlas *atlas, float sizePixels,
                                   const char *cjkOverride) noexcept;

} // namespace engine::editor
