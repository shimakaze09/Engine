// Declares the diagnostic screenshot writer: an uncompressed BGRA TGA
// encoder for the render backend's readback (ENGINE_BGFX_SCREENSHOT), kept
// free of image-codec dependencies and checked at every stdio step so a
// failed write is reported instead of leaving a silently truncated file.

#pragma once

#include <cstdint>

namespace engine::renderer {

/// Writes `height` rows of `width` BGRA8 texels (row `y` starts at
/// `data + y * pitch`) to `filePath` as an uncompressed top-left-origin
/// TGA; `yflip` writes the rows bottom-up. Returns false, after logging the
/// path and the failing step once, when the arguments are invalid, the
/// dimensions exceed TGA's 16-bit fields, or open/write/close fails; a
/// partial file may remain on disk in the write and close cases, which the
/// log line names. Diagnostic output only, so no staged replacement.
bool write_bgra_tga(const char *filePath, std::uint32_t width,
                    std::uint32_t height, std::uint32_t pitch,
                    const void *data, bool yflip) noexcept;

} // namespace engine::renderer
