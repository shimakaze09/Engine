// Verifies the diagnostic screenshot writer: a readback lands on disk as
// the exact TGA bytes (header, row order, yflip), and every failure the
// writer can meet (bad arguments, oversized dimensions, an unopenable
// path, a device that refuses the flush) is reported through the log
// instead of leaving a silently truncated or missing file.

#include "../test_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "screenshot_tga.h"

namespace {

namespace rr = engine::renderer;

constexpr const char *kOutputPath = "screenshot_tga_test_output.tga";

/// Log records the writer produced, and the last one's text.
struct LogTally final {
  std::size_t errors = 0U;
  char last[640] = {};
};

LogTally g_tally;

void tally_sink(engine::core::LogLevel level, const char *channel,
                const char *message, void *) noexcept {
  if ((level != engine::core::LogLevel::Error) || (channel == nullptr) ||
      (std::strcmp(channel, "renderer") != 0) || (message == nullptr)) {
    return;
  }
  ++g_tally.errors;
  std::snprintf(g_tally.last, sizeof(g_tally.last), "%s", message);
}

bool last_log_mentions(const char *needle) noexcept {
  return std::strstr(g_tally.last, needle) != nullptr;
}

/// Reads the whole output file; returns the byte count (0 when absent).
std::size_t read_output(std::uint8_t *buffer, std::size_t capacity) noexcept {
  std::FILE *file = std::fopen(kOutputPath, "rb");
  if (file == nullptr) {
    return 0U;
  }
  const std::size_t count = std::fread(buffer, 1U, capacity, file);
  std::fclose(file);
  return count;
}

/// A 2x2 BGRA readback whose pitch carries 4 bytes of padding per row, so
/// the writer must honor the pitch rather than assume packed rows.
constexpr std::uint32_t kWidth = 2U;
constexpr std::uint32_t kHeight = 2U;
constexpr std::uint32_t kPitch = 12U;
constexpr std::uint8_t kReadback[kHeight * kPitch] = {
    // row 0: two texels, then padding
    1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE,
    // row 1
    9, 10, 11, 12, 13, 14, 15, 16, 0xEE, 0xEE, 0xEE, 0xEE};

constexpr std::uint8_t kExpectedHeader[18] = {0, 0, 2, 0, 0, 0, 0,    0,   0,
                                              0, 0, 0, 2, 0, 2, 0, 32, 0x20};

void check_writes_exact_bytes(engine::tests::TestContext &ctx) {
  g_tally = LogTally{};
  ctx.check(rr::write_bgra_tga(kOutputPath, kWidth, kHeight, kPitch, kReadback,
                               false),
            "write: a valid readback succeeds");
  ctx.check(g_tally.errors == 0U, "write: success logs nothing");

  std::uint8_t bytes[64] = {};
  const std::size_t count = read_output(bytes, sizeof(bytes));
  ctx.check(count == 18U + (kWidth * kHeight * 4U),
            "write: the file holds the header and exactly the texel bytes");
  ctx.check(std::memcmp(bytes, kExpectedHeader, sizeof(kExpectedHeader)) == 0,
            "write: the header describes a 2x2 32-bit top-left TGA");
  const std::uint8_t topDown[] = {1, 2,  3,  4,  5,  6,  7,  8,
                                  9, 10, 11, 12, 13, 14, 15, 16};
  ctx.check(std::memcmp(bytes + 18, topDown, sizeof(topDown)) == 0,
            "write: rows follow the pitch, padding excluded, top row first");

  ctx.check(rr::write_bgra_tga(kOutputPath, kWidth, kHeight, kPitch, kReadback,
                               true),
            "yflip: a valid readback succeeds");
  const std::size_t flippedCount = read_output(bytes, sizeof(bytes));
  const std::uint8_t bottomUp[] = {9, 10, 11, 12, 13, 14, 15, 16,
                                   1, 2,  3,  4,  5,  6,  7,  8};
  ctx.check((flippedCount == 18U + (kWidth * kHeight * 4U)) &&
                (std::memcmp(bytes + 18, bottomUp, sizeof(bottomUp)) == 0),
            "yflip: rows are written bottom-up");
  static_cast<void>(std::remove(kOutputPath));
}

void check_rejects_bad_arguments(engine::tests::TestContext &ctx) {
  std::uint8_t probe[8] = {};

  g_tally = LogTally{};
  ctx.check(!rr::write_bgra_tga(nullptr, kWidth, kHeight, kPitch, kReadback,
                                false),
            "args: a null path fails");
  ctx.check(g_tally.errors == 1U && last_log_mentions("no output path"),
            "args: a null path is logged once");

  g_tally = LogTally{};
  ctx.check(!rr::write_bgra_tga(kOutputPath, kWidth, kHeight, kPitch, nullptr,
                                false),
            "args: null readback data fails");
  ctx.check(!rr::write_bgra_tga(kOutputPath, 0U, kHeight, kPitch, kReadback,
                                false),
            "args: a zero-width readback fails");
  ctx.check(g_tally.errors == 2U && last_log_mentions("empty readback"),
            "args: empty readbacks are logged");
  ctx.check(read_output(probe, sizeof(probe)) == 0U,
            "args: no file is created for an empty readback");

  g_tally = LogTally{};
  ctx.check(!rr::write_bgra_tga(kOutputPath, 70000U, 1U, 70000U * 4U,
                                kReadback, false),
            "args: a width beyond TGA's 16-bit field fails before any I/O");
  ctx.check(g_tally.errors == 1U && last_log_mentions("65535"),
            "args: the dimension limit is logged");
  ctx.check(read_output(probe, sizeof(probe)) == 0U,
            "args: no file is created for an oversized readback");

  g_tally = LogTally{};
  ctx.check(!rr::write_bgra_tga(kOutputPath, kWidth, kHeight, 4U, kReadback,
                                false),
            "args: a pitch narrower than one row fails");
  ctx.check(g_tally.errors == 1U && last_log_mentions("pitch"),
            "args: the pitch rejection is logged");
}

void check_reports_unopenable_path(engine::tests::TestContext &ctx) {
  g_tally = LogTally{};
  // A directory cannot be opened for writing on any platform.
  ctx.check(!rr::write_bgra_tga(".", kWidth, kHeight, kPitch, kReadback, false),
            "open: a directory path fails");
  ctx.check(g_tally.errors == 1U && last_log_mentions("cannot open"),
            "open: the failure names the path and the open step");
}

#if defined(__linux__)
/// /dev/full accepts the buffered writes and fails the flush at close with
/// ENOSPC: the full-disk shape the diagnostic must report.
void check_reports_failed_flush(engine::tests::TestContext &ctx) {
  g_tally = LogTally{};
  ctx.check(!rr::write_bgra_tga("/dev/full", kWidth, kHeight, kPitch,
                                kReadback, false),
            "full disk: the write reports failure");
  ctx.check(g_tally.errors == 1U && (last_log_mentions("short write") ||
                                     last_log_mentions("close failed")),
            "full disk: the failing step is logged once");
}
#endif

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&tally_sink, nullptr)) {
    return 1;
  }
  static_cast<void>(std::remove(kOutputPath));

  engine::tests::TestContext ctx;
  check_writes_exact_bytes(ctx);
  check_rejects_bad_arguments(ctx);
  check_reports_unopenable_path(ctx);
#if defined(__linux__)
  check_reports_failed_flush(ctx);
#endif

  engine::core::log_unregister_sink(&tally_sink, nullptr);
  static_cast<void>(std::remove(kOutputPath));
  return ctx.finish("screenshot_tga");
}
