// Implements Game view screenshots in the editor: naming the file, turning
// the Game view's rectangle into back-buffer pixels, deferring the capture
// by one frame so nothing the editor draws over the view is in it, and the
// `screenshot` console command.

#include "editor_screenshot.h"

#include "editor_session.h"
#include "editor_shortcuts.h"

#include "engine/core/atomic_file.h"
#include "engine/core/console.h"
#include "engine/core/logging.h"
#include "engine/core/project_data.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace engine::editor {

namespace {

constexpr int kMaxNameSuffix = 99;
constexpr int kNoFrame = -1;
/// Frames a request waits for a hidden Game view to come to the front.
constexpr int kMaxShowFrames = 10;

/// The ImGui frame a pending request was made in, the frame it is being
/// taken in, and the first and latest frames of the Game view's current
/// run of shown frames; kNoFrame when none.
int g_requestFrame = kNoFrame;
int g_captureFrame = kNoFrame;
int g_shownSince = kNoFrame;
int g_lastShown = kNoFrame;

// Paths are UTF-8, which every executable's narrow code page is (on
// Windows through its manifest), so a path converts as it stands.
bool file_exists(const char *path, void *) noexcept {
  std::error_code ec{};
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

int current_frame() noexcept {
  return (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetFrameCount() : 0;
}

void screenshot_command(const char *const *, int, void *) noexcept {
  request_game_view_screenshot();
  core::console_print("screenshot: taking the Game view");
}

} // namespace

bool next_screenshot_path(const char *directory, const std::tm &time,
                          ScreenshotPathExistsFn exists, void *userData,
                          char *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((directory == nullptr) || (exists == nullptr)) {
    return false;
  }
  char stamp[32] = {};
  std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
                time.tm_hour, time.tm_min, time.tm_sec);
  for (int suffix = 1; suffix <= kMaxNameSuffix; ++suffix) {
    char suffixText[8] = {};
    if (suffix > 1) {
      std::snprintf(suffixText, sizeof(suffixText), "-%d", suffix);
    }
    const int written =
        std::snprintf(out, capacity, "%s/Screenshot-%s%s.png", directory,
                      stamp, suffixText);
    if ((written < 0) || (static_cast<std::size_t>(written) >= capacity)) {
      out[0] = '\0';
      return false;
    }
    if (!exists(out, userData)) {
      return true;
    }
  }
  out[0] = '\0';
  return false;
}

renderer::ScreenshotRegion
game_view_screenshot_region(ImVec2 screenPos, ImVec2 size, ImVec2 viewportPos,
                            ImVec2 framebufferScale) noexcept {
  renderer::ScreenshotRegion region{};
  region.x = static_cast<std::int32_t>(
      std::lround((screenPos.x - viewportPos.x) * framebufferScale.x));
  region.y = static_cast<std::int32_t>(
      std::lround((screenPos.y - viewportPos.y) * framebufferScale.y));
  region.width = static_cast<std::int32_t>(size.x * framebufferScale.x);
  region.height = static_cast<std::int32_t>(size.y * framebufferScale.y);
  return region;
}

void request_game_view_screenshot() noexcept {
  if (g_requestFrame == kNoFrame) {
    g_requestFrame = current_frame();
  }
}

bool begin_game_view_screenshot_frame(bool visible) noexcept {
  const int frame = current_frame();
  if (!visible) {
    g_shownSince = kNoFrame;
  } else if ((g_shownSince == kNoFrame) ||
             ((g_lastShown != frame) && (g_lastShown != frame - 1))) {
    g_shownSince = frame;
  }
  if (visible) {
    g_lastShown = frame;
  }
  if (g_captureFrame == frame) {
    return true;
  }
  if ((g_requestFrame == kNoFrame) || (frame <= g_requestFrame)) {
    return false;
  }
  if (!visible) {
    if (frame - g_requestFrame > kMaxShowFrames) {
      g_requestFrame = kNoFrame;
      core::log_message(core::LogLevel::Warning, "editor",
                        "screenshot dropped: the Game view could not be "
                        "shown");
      return false;
    }
    // A tab behind another comes to the front, as Play brings it.
    editor_session().pendingViewFocus = kGameViewWindow;
    return false;
  }
  // The Game view renders only while shown, so its image is this size's
  // picture from its second shown frame on.
  if (frame == g_shownSince) {
    return false;
  }
  g_requestFrame = kNoFrame;
  g_captureFrame = frame;
  return true;
}

bool game_view_screenshot_capturing() noexcept {
  return (g_captureFrame != kNoFrame) && (g_captureFrame == current_frame());
}

bool take_game_view_screenshot(
    const renderer::ScreenshotRegion &region) noexcept {
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    return false; // project_data_dir logged why
  }
  const std::size_t length = std::strlen(directory);
  static constexpr char kSubdirectory[] = "/Screenshots";
  if (length + sizeof(kSubdirectory) > sizeof(directory)) {
    core::log_message(core::LogLevel::Error, "editor",
                      "screenshot refused: the project data path is too long");
    return false;
  }
  std::memcpy(directory + length, kSubdirectory, sizeof(kSubdirectory));
  if (!core::create_directories_durably(directory)) {
    char message[1100] = {};
    std::snprintf(message, sizeof(message),
                  "screenshot refused: cannot create %s", directory);
    core::log_message(core::LogLevel::Error, "editor", message);
    return false;
  }
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char path[renderer::kMaxScreenshotPath] = {};
  if (!next_screenshot_path(directory, local, &file_exists, nullptr, path,
                            sizeof(path))) {
    core::log_message(core::LogLevel::Error, "editor",
                      "screenshot refused: no free file name fits under the "
                      "project's Screenshots directory");
    return false;
  }
  return renderer::request_screenshot(path, &region);
}

bool register_screenshot_command() noexcept {
  return core::console_register_command(
      "screenshot", &screenshot_command, nullptr,
      "Save the Game view as a PNG in the project's Screenshots folder");
}

} // namespace engine::editor
