// Implements the diagnostic screenshot writer (uncompressed BGRA TGA) with
// every stdio result checked, so a full disk or a bad path is one logged
// error rather than a truncated image nobody is told about.

#include "screenshot_tga.h"

#include <cstddef>
#include <cstdio>

#include "engine/core/logging.h"

namespace engine::renderer {

namespace {

constexpr std::uint32_t kMaxTgaDimension = 0xFFFFU;
constexpr std::size_t kBytesPerTexel = 4U;

/// One log line per failure: `<path>: <reason>`.
void log_screenshot_failure(const char *filePath, const char *reason) noexcept {
  char message[640] = {};
  std::snprintf(message, sizeof(message), "screenshot %s: %s",
                (filePath != nullptr) ? filePath : "(null)", reason);
  core::log_message(core::LogLevel::Error, "renderer", message);
}

std::FILE *open_for_write(const char *filePath) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, filePath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(filePath, "wb");
#endif
  return file;
}

} // namespace

bool write_bgra_tga(const char *filePath, std::uint32_t width,
                    std::uint32_t height, std::uint32_t pitch,
                    const void *data, bool yflip) noexcept {
  if ((filePath == nullptr) || (filePath[0] == '\0')) {
    log_screenshot_failure(filePath, "no output path");
    return false;
  }
  if ((data == nullptr) || (width == 0U) || (height == 0U)) {
    log_screenshot_failure(filePath, "empty readback");
    return false;
  }
  if ((width > kMaxTgaDimension) || (height > kMaxTgaDimension)) {
    log_screenshot_failure(filePath,
                           "readback exceeds TGA's 65535-texel dimensions");
    return false;
  }
  const std::size_t rowBytes = static_cast<std::size_t>(width) * kBytesPerTexel;
  if (static_cast<std::size_t>(pitch) < rowBytes) {
    log_screenshot_failure(filePath, "readback pitch is narrower than a row");
    return false;
  }

  std::FILE *file = open_for_write(filePath);
  if (file == nullptr) {
    log_screenshot_failure(filePath, "cannot open for writing");
    return false;
  }

  std::uint8_t header[18] = {};
  header[2] = 2; // uncompressed true-color
  header[12] = static_cast<std::uint8_t>(width & 0xFFU);
  header[13] = static_cast<std::uint8_t>((width >> 8U) & 0xFFU);
  header[14] = static_cast<std::uint8_t>(height & 0xFFU);
  header[15] = static_cast<std::uint8_t>((height >> 8U) & 0xFFU);
  header[16] = 32;   // BGRA
  header[17] = 0x20; // top-left origin

  bool written = std::fwrite(header, 1U, sizeof(header), file) == sizeof(header);
  const auto *rows = static_cast<const std::uint8_t *>(data);
  for (std::uint32_t y = 0U; written && (y < height); ++y) {
    const std::uint32_t row = yflip ? (height - 1U - y) : y;
    written = std::fwrite(rows + (static_cast<std::size_t>(row) * pitch), 1U,
                          rowBytes, file) == rowBytes;
  }
  // The close flushes the stdio buffer, so a full disk surfaces here as
  // often as in the writes; both leave a partial file behind.
  const bool closed = std::fclose(file) == 0;
  if (!written) {
    log_screenshot_failure(filePath, "short write; the file is incomplete");
    return false;
  }
  if (!closed) {
    log_screenshot_failure(filePath,
                           "close failed; the file may be incomplete");
    return false;
  }
  return true;
}

} // namespace engine::renderer
