// Reads rendered pixels back for the gpu-labelled tests: asks the bgfx
// backend to write the presented back buffer as a TGA, runs production
// frames until it lands, and loads it. This is the only way a test can
// observe what the GPU drew — the fake-device suites assert which calls
// were made, never what the image holds, so a pass that binds the right
// resources and still addresses them wrongly is invisible to them.

#pragma once

#include "engine/runtime/engine_pipeline.h"

#include "render_device_bgfx.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace engine::tests {

/// A captured frame: tightly packed BGRA rows, top row first.
struct CapturedFrame final {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::vector<std::uint8_t> bgra;

  /// The blue, green or red byte (channel 0, 1, 2) of one pixel.
  std::uint8_t channel(std::uint32_t x, std::uint32_t y,
                       std::uint32_t index) const noexcept {
    return bgra[(static_cast<std::size_t>(y) * width + x) * 4U + index];
  }
};

/// Loads the uncompressed 32-bit top-left-origin TGA the backend writes.
/// False for anything else, including a file still being written.
inline bool load_captured_tga(const char *path, CapturedFrame *out) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }
  std::uint8_t header[18] = {};
  bool ok = std::fread(header, 1U, sizeof(header), file) == sizeof(header);
  ok = ok && (header[2] == 2U) && (header[16] == 32U) && (header[17] == 0x20U);
  if (ok) {
    out->width = static_cast<std::uint32_t>(header[12]) |
                 (static_cast<std::uint32_t>(header[13]) << 8U);
    out->height = static_cast<std::uint32_t>(header[14]) |
                  (static_cast<std::uint32_t>(header[15]) << 8U);
    const std::size_t bytes =
        static_cast<std::size_t>(out->width) * out->height * 4U;
    ok = bytes > 0U;
    if (ok) {
      out->bgra.assign(bytes, 0U);
      ok = std::fread(out->bgra.data(), 1U, bytes, file) == bytes;
    }
  }
  static_cast<void>(std::fclose(file));
  return ok;
}

/// Requests a back-buffer capture and runs production frames until the
/// file lands. The wait is counted in frames, never in time: bgfx returns
/// the pixels a frame or two after the request, and a capture that has
/// not arrived within maxFrames is a failure, not something to wait out.
inline bool capture_presented_frame(engine::EnginePipeline &pipeline,
                                    const char *path, CapturedFrame *out,
                                    int maxFrames = 16) noexcept {
  std::error_code ec{};
  std::filesystem::remove(path, ec);
  if (!engine::renderer::render_device_bgfx_request_screenshot(path)) {
    return false;
  }
  for (int frame = 0; frame < maxFrames; ++frame) {
    if (!pipeline.execute_frame()) {
      return false;
    }
    // The file is left in place: a failing run is diagnosed by looking
    // at the frames it compared.
    if (std::filesystem::exists(path, ec) && load_captured_tga(path, out)) {
      return true;
    }
  }
  return false;
}

/// Mean absolute per-channel difference over a rectangle, in 8-bit
/// levels. The rectangle is clamped to the smaller frame; frames of
/// different sizes compare as maximally different.
inline double mean_abs_difference(const CapturedFrame &a, const CapturedFrame &b,
                                  std::uint32_t x0, std::uint32_t y0,
                                  std::uint32_t x1, std::uint32_t y1) noexcept {
  if ((a.width != b.width) || (a.height != b.height) || (x1 <= x0) ||
      (y1 <= y0) || (x1 > a.width) || (y1 > a.height)) {
    return 255.0;
  }
  std::uint64_t sum = 0U;
  std::uint64_t samples = 0U;
  for (std::uint32_t y = y0; y < y1; ++y) {
    for (std::uint32_t x = x0; x < x1; ++x) {
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        const int da = a.channel(x, y, c);
        const int db = b.channel(x, y, c);
        sum += static_cast<std::uint64_t>((da > db) ? (da - db) : (db - da));
        ++samples;
      }
    }
  }
  return static_cast<double>(sum) / static_cast<double>(samples);
}

/// Mean of one channel (0 blue, 1 green, 2 red) over a rectangle.
inline double mean_channel(const CapturedFrame &frame, std::uint32_t index,
                           std::uint32_t x0, std::uint32_t y0,
                           std::uint32_t x1, std::uint32_t y1) noexcept {
  if ((x1 <= x0) || (y1 <= y0) || (x1 > frame.width) || (y1 > frame.height)) {
    return 0.0;
  }
  std::uint64_t sum = 0U;
  for (std::uint32_t y = y0; y < y1; ++y) {
    for (std::uint32_t x = x0; x < x1; ++x) {
      sum += frame.channel(x, y, index);
    }
  }
  return static_cast<double>(sum) /
         static_cast<double>(static_cast<std::uint64_t>(x1 - x0) * (y1 - y0));
}

} // namespace engine::tests
