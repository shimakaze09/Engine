// Loads the editor's UI fonts: Roboto for Latin text, with a CJK-capable
// face merged behind it so Chinese and Japanese names, folders and field
// text render instead of drawing as missing-glyph boxes.
//
// No CJK face ships with the engine. The editor uses the operating
// system's own (Microsoft YaHei on Windows, PingFang or Hiragino on macOS,
// Noto CJK or WenQuanYi on Linux), and `editor.cjk_font` names any other
// file to use first.

#pragma once

#include <cstddef>

struct ImFontAtlas;

namespace engine::editor {

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
/// non-empty, is tried before the system candidates. Needs a renderer that
/// honours ImGui's texture requests, since glyphs are rasterized as they
/// are first drawn rather than baked from fixed ranges. Logs what it could
/// not load; never fails the editor over a font.
EditorFontResult load_editor_fonts(ImFontAtlas *atlas, float sizePixels,
                                   const char *cjkOverride) noexcept;

} // namespace engine::editor
