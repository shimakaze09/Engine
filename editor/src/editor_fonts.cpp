// Implements the editor font chain declared in editor_fonts.h.

#include "editor_fonts.h"

#include "engine/core/logging.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <limits>

namespace engine::editor {

namespace {

constexpr const char *kLatinFontPath = "assets/fonts/Roboto-Medium.ttf";

#if defined(_WIN32)
constexpr const char *kCjkCandidates[] = {
    "C:/Windows/Fonts/msyh.ttc",    "C:/Windows/Fonts/msyh.ttf",
    "C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc",
    "C:/Windows/Fonts/simhei.ttf",  "C:/Windows/Fonts/simsun.ttc",
};
#elif defined(__APPLE__)
constexpr const char *kCjkCandidates[] = {
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
};
#else
constexpr const char *kCjkCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
};
#endif

// ImGui asserts on font data this size or smaller, which in an
// assert-enabled build would let a mistyped editor.cjk_font abort the
// editor; no real font is anywhere near it.
constexpr std::size_t kMinFontBytes = 100U;

/// Reads a whole font file into ImGui-owned memory; null when it cannot be
/// read, is too small to be a font, or does not fit the int size ImGui
/// takes.
void *read_font(const char *path, int *outBytes) noexcept {
  std::size_t bytes = 0U;
  void *data = ImFileLoadToMemory(path, "rb", &bytes);
  if (data == nullptr) {
    return nullptr;
  }
  if ((bytes <= kMinFontBytes) ||
      (bytes > static_cast<std::size_t>(std::numeric_limits<int>::max()))) {
    IM_FREE(data);
    return nullptr;
  }
  *outBytes = static_cast<int>(bytes);
  return data;
}

/// Merges the font at `path` into the atlas's last font. The atlas owns
/// the bytes from here on, whether or not ImGui accepts them.
bool merge_font(ImFontAtlas *atlas, const char *path,
                float sizePixels) noexcept {
  int bytes = 0;
  void *data = read_font(path, &bytes);
  if (data == nullptr) {
    return false;
  }
  ImFontConfig config{};
  config.MergeMode = true;
  return atlas->AddFontFromMemoryTTF(data, bytes, sizePixels, &config) !=
         nullptr;
}

} // namespace

const char *const *editor_cjk_font_candidates(std::size_t *outCount) noexcept {
  if (outCount != nullptr) {
    *outCount = sizeof(kCjkCandidates) / sizeof(kCjkCandidates[0]);
  }
  return kCjkCandidates;
}

EditorFontResult load_editor_fonts(ImFontAtlas *atlas, float sizePixels,
                                   const char *cjkOverride) noexcept {
  EditorFontResult result{};
  if (atlas == nullptr) {
    return result;
  }

  // Read here and handed over as memory: ImGui's own path-based loader
  // asserts on an unreadable file in assert-enabled builds, which would
  // turn a missing asset into an abort instead of the fallback below.
  int latinBytes = 0;
  void *latin = read_font(kLatinFontPath, &latinBytes);
  if (latin != nullptr) {
    result.latin =
        atlas->AddFontFromMemoryTTF(latin, latinBytes, sizePixels) != nullptr;
  }
  if (!result.latin) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "editor font missing; using ImGui default");
    // The merge needs a font to merge into, and at an explicit size: ImGui
    // refuses to merge a sized face into a font whose size was implied.
    ImFontConfig fallback{};
    fallback.SizePixels = sizePixels;
    atlas->AddFontDefault(&fallback);
  }

  const bool hasOverride = (cjkOverride != nullptr) && (cjkOverride[0] != '\0');
  if (hasOverride && merge_font(atlas, cjkOverride, sizePixels)) {
    result.cjk = true;
    std::snprintf(result.cjkPath, sizeof(result.cjkPath), "%s", cjkOverride);
  } else {
    if (hasOverride) {
      char message[640] = {};
      std::snprintf(message, sizeof(message),
                    "editor.cjk_font could not be loaded; trying the system "
                    "fonts: %s",
                    cjkOverride);
      core::log_message(core::LogLevel::Warning, "editor", message);
    }
    for (const char *candidate : kCjkCandidates) {
      if (merge_font(atlas, candidate, sizePixels)) {
        result.cjk = true;
        std::snprintf(result.cjkPath, sizeof(result.cjkPath), "%s", candidate);
        break;
      }
    }
  }

  if (result.cjk) {
    char message[640] = {};
    std::snprintf(message, sizeof(message), "editor CJK font: %s",
                  result.cjkPath);
    core::log_message(core::LogLevel::Info, "editor", message);
  } else {
    core::log_message(core::LogLevel::Warning, "editor",
                      "no CJK font found; Chinese and Japanese text will "
                      "draw as boxes. Set editor.cjk_font to a .ttf/.ttc/"
                      ".otf file that has them");
  }
  return result;
}

} // namespace engine::editor
