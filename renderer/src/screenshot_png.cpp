// Implements the screenshot PNG writer: crop, swizzle BGRA to RGB, encode
// with stb_image_write in memory, and commit through the staged atomic
// write.

#include "screenshot_png.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // sprintf deprecated
#pragma warning(disable : 4244) // int to short conversions
#pragma warning(disable : 4505) // unreferenced static encoders
#endif
// Static: the asset packer compiles its own copy of the encoder, and the
// two must not collide when a program links both.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace engine::renderer {

namespace {

constexpr std::size_t kBytesPerTexel = 4U;
constexpr std::size_t kBytesPerPngTexel = 3U;

void log_failure(const char *filePath, const char *reason) noexcept {
  char message[640] = {};
  std::snprintf(message, sizeof(message), "screenshot %s: %s",
                (filePath != nullptr) ? filePath : "(null)", reason);
  core::log_message(core::LogLevel::Error, "renderer", message);
}

} // namespace

bool write_bgra_png(const char *filePath, std::uint32_t width,
                    std::uint32_t height, std::uint32_t pitch,
                    const void *data, bool yflip,
                    const ScreenshotRegion *region) noexcept {
  if ((filePath == nullptr) || (filePath[0] == '\0')) {
    log_failure(filePath, "no output path");
    return false;
  }
  if ((data == nullptr) || (width == 0U) || (height == 0U)) {
    log_failure(filePath, "empty readback");
    return false;
  }
  if (static_cast<std::size_t>(pitch) <
      static_cast<std::size_t>(width) * kBytesPerTexel) {
    log_failure(filePath, "readback pitch is narrower than a row");
    return false;
  }

  // The rectangle to keep, clamped to the image, in top-down rows.
  std::int64_t x0 = 0;
  std::int64_t y0 = 0;
  std::int64_t x1 = width;
  std::int64_t y1 = height;
  if (region != nullptr) {
    x0 = std::max<std::int64_t>(region->x, 0);
    y0 = std::max<std::int64_t>(region->y, 0);
    x1 = std::min<std::int64_t>(static_cast<std::int64_t>(region->x) +
                                    region->width,
                                width);
    y1 = std::min<std::int64_t>(static_cast<std::int64_t>(region->y) +
                                    region->height,
                                height);
  }
  if ((x1 <= x0) || (y1 <= y0)) {
    log_failure(filePath, "the region lies outside the frame");
    return false;
  }
  const std::size_t outWidth = static_cast<std::size_t>(x1 - x0);
  const std::size_t outHeight = static_cast<std::size_t>(y1 - y0);

  auto *rgb = new (std::nothrow)
      std::uint8_t[outWidth * outHeight * kBytesPerPngTexel];
  if (rgb == nullptr) {
    log_failure(filePath, "out of memory for the image");
    return false;
  }
  const auto *rows = static_cast<const std::uint8_t *>(data);
  for (std::size_t y = 0U; y < outHeight; ++y) {
    const std::size_t topDown = static_cast<std::size_t>(y0) + y;
    const std::size_t sourceRow =
        yflip ? (static_cast<std::size_t>(height) - 1U - topDown) : topDown;
    const std::uint8_t *source = rows +
                                 (sourceRow * static_cast<std::size_t>(pitch)) +
                                 (static_cast<std::size_t>(x0) * kBytesPerTexel);
    std::uint8_t *target = rgb + (y * outWidth * kBytesPerPngTexel);
    for (std::size_t x = 0U; x < outWidth; ++x) {
      target[(x * kBytesPerPngTexel) + 0U] = source[(x * kBytesPerTexel) + 2U];
      target[(x * kBytesPerPngTexel) + 1U] = source[(x * kBytesPerTexel) + 1U];
      target[(x * kBytesPerPngTexel) + 2U] = source[(x * kBytesPerTexel) + 0U];
    }
  }

  int encodedSize = 0;
  unsigned char *encoded = stbi_write_png_to_mem(
      rgb, static_cast<int>(outWidth * kBytesPerPngTexel),
      static_cast<int>(outWidth), static_cast<int>(outHeight),
      static_cast<int>(kBytesPerPngTexel), &encodedSize);
  delete[] rgb;
  if ((encoded == nullptr) || (encodedSize <= 0)) {
    std::free(encoded);
    log_failure(filePath, "the image could not be encoded");
    return false;
  }
  const bool written = core::atomic_write_file(
      filePath, encoded, static_cast<std::size_t>(encodedSize));
  std::free(encoded);
  if (!written) {
    log_failure(filePath, "the file could not be written");
    return false;
  }
  return true;
}

} // namespace engine::renderer
