// Declares screenshots: a request that the next presented frame, or a
// rectangle of it, be saved as a PNG, and the result of the last one. The
// capture reads the back buffer, which in the editor holds the whole window,
// so a caller that wants one panel passes that panel's rectangle.

#pragma once

#include <cstdint>

namespace engine::renderer {

/// A rectangle of the back buffer, in pixels, from its top-left corner.
struct ScreenshotRegion final {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
};

/// Longest screenshot path, terminator included.
inline constexpr std::uint32_t kMaxScreenshotPath = 512U;

/// Asks for the next presented frame to be saved to `path` as a PNG, cropped
/// to `region` when one is given (clamped to the frame; a region wholly off
/// it fails the save). The pixels arrive a frame or two later; the file is
/// written whole or not at all, and the log names it either way. False,
/// with a log line, when there is no rendering device to read back from, a
/// request is already waiting for its frame, or the path is empty or does
/// not fit kMaxScreenshotPath.
bool request_screenshot(const char *path,
                        const ScreenshotRegion *region) noexcept;

/// The outcome of the last finished screenshot request.
struct ScreenshotResult final {
  /// Finished requests so far; 0 before the first.
  std::uint32_t sequence = 0U;
  bool succeeded = false;
  char path[kMaxScreenshotPath] = {};
};

/// The last finished request's outcome.
ScreenshotResult last_screenshot_result() noexcept;

} // namespace engine::renderer
