// Declares the screenshot PNG writer: the render backend's BGRA readback,
// optionally cropped to a rectangle, encoded as an opaque RGB PNG in memory
// and written through the staged atomic file write, so a failed save never
// leaves a partial image behind.

#pragma once

#include <cstdint>

#include "engine/renderer/screenshot.h"

namespace engine::renderer {

/// Writes the `width` x `height` BGRA8 readback (row `y` starts at
/// `data + y * pitch`; `yflip` means the rows run bottom-up) to `filePath`
/// as a PNG, cropped to `region` when one is given. The region is clamped
/// to the image; one with nothing left fails. The alpha channel is dropped:
/// a back buffer's alpha is not an opacity. Returns false, after logging the
/// path and the reason once, when the arguments are invalid, the image
/// cannot be encoded, or the file cannot be written; the previous file at
/// the path, if any, is then left as it was.
bool write_bgra_png(const char *filePath, std::uint32_t width,
                    std::uint32_t height, std::uint32_t pitch,
                    const void *data, bool yflip,
                    const ScreenshotRegion *region) noexcept;

} // namespace engine::renderer
