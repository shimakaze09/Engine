// Verifies texture loader test behavior for the Engine test suite.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <system_error>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/texture_loader.h"

namespace {

int check_init_shutdown() {
  const bool initOk = engine::renderer::initialize_texture_system();
  if (!initOk) {
    return 11;
  }

  // Double init should succeed.
  const bool initAgain = engine::renderer::initialize_texture_system();
  if (!initAgain) {
    return 12;
  }

  engine::renderer::shutdown_texture_system();
  return 0;
}

int check_null_path() {
  const bool initOk = engine::renderer::initialize_texture_system();
  if (!initOk) {
    return 21;
  }

  const engine::renderer::TextureHandle handle =
      engine::renderer::load_texture(nullptr);
  if (handle != engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    return 22;
  }

  engine::renderer::shutdown_texture_system();
  return 0;
}

int check_invalid_handle() {
  const bool initOk = engine::renderer::initialize_texture_system();
  if (!initOk) {
    return 31;
  }

  const engine::renderer::DeviceTextureHandle deviceTexture =
      engine::renderer::texture_device_handle(
          engine::renderer::kInvalidTextureHandle);
  if (deviceTexture != engine::renderer::kInvalidDeviceTexture) {
    engine::renderer::shutdown_texture_system();
    return 32;
  }

  const bool hdr =
      engine::renderer::is_texture_hdr(engine::renderer::kInvalidTextureHandle);
  if (hdr) {
    engine::renderer::shutdown_texture_system();
    return 33;
  }

  const bool cubemap = engine::renderer::is_texture_cubemap(
      engine::renderer::kInvalidTextureHandle);
  if (cubemap) {
    engine::renderer::shutdown_texture_system();
    return 34;
  }

  // Unloading invalid handle should not crash.
  engine::renderer::unload_texture(engine::renderer::kInvalidTextureHandle);

  engine::renderer::shutdown_texture_system();
  return 0;
}

int check_load_before_init() {
  // Loading before init should return invalid.
  engine::renderer::shutdown_texture_system();
  const engine::renderer::TextureHandle handle =
      engine::renderer::load_texture("assets/textures/test.png");
  if (handle != engine::renderer::kInvalidTextureHandle) {
    return 41;
  }

  return 0;
}

int check_stb_input_size_validation() {
  int stbSize = -1;
  const auto maxSize =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (!engine::renderer::texture_input_size_fits_stb(maxSize, &stbSize)) {
    return 51;
  }
  if (stbSize != std::numeric_limits<int>::max()) {
    return 52;
  }

  stbSize = 123;
  if (engine::renderer::texture_input_size_fits_stb(maxSize + 1U,
                                                    &stbSize)) {
    return 53;
  }
  if (stbSize != 123) {
    return 54;
  }

  if (engine::renderer::texture_input_size_fits_stb(1U, nullptr)) {
    return 55;
  }

  return 0;
}


int check_texture_asset_database() {
  // Basic texture asset database test.
  // TextureAssetRecord is already compiled into asset_database;
  // this just verifies the struct is usable.
  engine::renderer::TextureAssetRecord record{};
  if (record.id != engine::renderer::kInvalidAssetId) {
    return 51;
  }

  if (record.runtimeTexture != engine::renderer::kInvalidTextureHandle) {
    return 52;
  }

  if (record.state != engine::renderer::AssetState::Unloaded) {
    return 53;
  }

  return 0;
}

int check_cubemap_invalid_args() {
  const bool initOk = engine::renderer::initialize_texture_system();
  if (!initOk) {
    return 61;
  }

  engine::renderer::TextureHandle handle =
      engine::renderer::load_hdr_equirect_cubemap(nullptr, 64);
  if (handle != engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    return 62;
  }

  handle = engine::renderer::load_hdr_equirect_cubemap(
      "assets/textures/test.hdr", 0);
  if (handle != engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    return 63;
  }

  engine::renderer::shutdown_texture_system();
  return 0;
}

int check_skybox_assignment() {
  engine::renderer::TextureHandle handle{};
  handle.id = 123U;
  engine::renderer::set_skybox_texture(handle);
  if (engine::renderer::get_skybox_texture() != handle) {
    return 71;
  }

  engine::renderer::set_skybox_texture(engine::renderer::kInvalidTextureHandle);
  if (engine::renderer::get_skybox_texture() !=
      engine::renderer::kInvalidTextureHandle) {
    return 72;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
// ---- Decode-budget preflight (audit #210) ---------------------------------

/// Captured log lines for asserting which rejection fired.
char g_capturedLog[4096] = {};

void capture_log_sink(engine::core::LogLevel, const char *,
                      const char *message, void *) noexcept {
  if (message == nullptr) {
    return;
  }
  const std::size_t used = std::strlen(g_capturedLog);
  std::snprintf(g_capturedLog + used, sizeof(g_capturedLog) - used, "%s\n",
                message);
}

bool captured_log_contains(const char *needle) noexcept {
  return std::strstr(g_capturedLog, needle) != nullptr;
}

bool write_test_file(const char *path, const void *data,
                     std::size_t size) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool ok = std::fwrite(data, 1U, size, file) == size;
  return (std::fclose(file) == 0) && ok;
}

/// Writes a Radiance .hdr file that claims the given dimensions but
/// carries no pixel data — the header alone must trip the budget gate.
bool write_hdr_header(const char *path, int width, int height) noexcept {
  char text[128] = {};
  const int written =
      std::snprintf(text, sizeof(text),
                    "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n",
                    height, width);
  return (written > 0) &&
         write_test_file(path, text, static_cast<std::size_t>(written));
}

void put_u32le(unsigned char *out, std::uint32_t value) noexcept {
  out[0] = static_cast<unsigned char>(value & 0xFFU);
  out[1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
  out[2] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
  out[3] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
}

/// Builds a 24-bit BMP header claiming the given dimensions, plus
/// `payloadBytes` of zero pixel data (0 = header-only hostile file).
std::size_t build_bmp(unsigned char *out, std::size_t capacity, int width,
                      int height, std::size_t payloadBytes) noexcept {
  const std::size_t total = 54U + payloadBytes;
  if (capacity < total) {
    return 0U;
  }
  std::memset(out, 0, total);
  out[0] = 'B';
  out[1] = 'M';
  put_u32le(out + 2, static_cast<std::uint32_t>(total));
  put_u32le(out + 10, 54U); // pixel data offset
  put_u32le(out + 14, 40U); // BITMAPINFOHEADER size
  put_u32le(out + 18, static_cast<std::uint32_t>(width));
  put_u32le(out + 22, static_cast<std::uint32_t>(height));
  out[26] = 1U; // planes
  out[28] = 24U; // bits per pixel
  return total;
}

/// The exposed preflight accepts a real in-budget image and rejects
/// garbage, and the production loader rejects over-budget dimensions and
/// decoded-byte totals from the header alone — reported through the
/// decode-budget diagnostic, not a generic decode failure.
int check_decode_budget() {
  // Direct preflight: a real 2x2 BMP passes and reports its decoded size.
  unsigned char bmp[128] = {};
  const std::size_t tinySize = build_bmp(bmp, sizeof(bmp), 2, 2, 16U);
  if (tinySize == 0U) {
    return 61;
  }
  std::uint64_t decodedBytes = 0ULL;
  if (!engine::renderer::texture_decode_within_budget(
          bmp, static_cast<int>(tinySize), false, 0, "tiny.bmp",
          &decodedBytes)) {
    return 62;
  }
  if (decodedBytes != (2ULL * 2ULL * 3ULL)) {
    return 63;
  }
  const unsigned char garbage[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  if (engine::renderer::texture_decode_within_budget(
          garbage, sizeof(garbage), false, 0, "garbage", nullptr)) {
    return 64;
  }

  // Production loads through a mounted directory of hostile headers.
  const char *kDir = "texture_budget_work";
  std::error_code ec{};
  std::filesystem::create_directories(kDir, ec);
  if (ec || !engine::core::mount("texbudget", kDir)) {
    return 65;
  }
  if (!engine::renderer::initialize_texture_system()) {
    return 66;
  }
  // log_message drops everything (sinks included) before initialization.
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&capture_log_sink, nullptr)) {
    return 67;
  }

  int result = 0;
  unsigned char hostile[128] = {};
  const std::size_t hugeBmpSize =
      build_bmp(hostile, sizeof(hostile), 30000, 30000, 0U);

  // Dimension cap: 20000-wide HDR header.
  if (!write_hdr_header("texture_budget_work/wide.hdr", 20000, 100)) {
    result = 68;
  } else if (engine::renderer::load_texture("texbudget/wide.hdr") !=
             engine::renderer::kInvalidTextureHandle) {
    result = 69;
  } else if (!captured_log_contains("decode budget (dimension")) {
    result = 70;
  }

  // Decoded-byte cap: 12000x12000 HDR fits the dimension cap but decodes
  // to ~1.7 GiB of floats.
  g_capturedLog[0] = '\0';
  if (result == 0) {
    if (!write_hdr_header("texture_budget_work/big.hdr", 12000, 12000)) {
      result = 71;
    } else if (engine::renderer::load_texture("texbudget/big.hdr") !=
               engine::renderer::kInvalidTextureHandle) {
      result = 72;
    } else if (!captured_log_contains("decode budget (decoded bytes")) {
      result = 73;
    }
  }

  // LDR dimension cap through a hostile BMP header.
  g_capturedLog[0] = '\0';
  if (result == 0) {
    if ((hugeBmpSize == 0U) ||
        !write_test_file("texture_budget_work/huge.bmp", hostile,
                         hugeBmpSize)) {
      result = 74;
    } else if (engine::renderer::load_texture("texbudget/huge.bmp") !=
               engine::renderer::kInvalidTextureHandle) {
      result = 75;
    } else if (!captured_log_contains("decode budget (dimension")) {
      result = 76;
    }
  }

  // The equirect path rejects from the header too.
  g_capturedLog[0] = '\0';
  if (result == 0) {
    if (engine::renderer::load_hdr_equirect_cubemap("texbudget/big.hdr",
                                                    512) !=
        engine::renderer::kInvalidTextureHandle) {
      result = 77;
    } else if (!captured_log_contains("decode budget (decoded bytes")) {
      result = 78;
    }
  }

  // Boundary: an in-budget real image passes the gate (headless, the load
  // then fails only at GPU-texture creation — never with a budget error).
  g_capturedLog[0] = '\0';
  if (result == 0) {
    if (!write_test_file("texture_budget_work/tiny.bmp", bmp, tinySize)) {
      result = 79;
    } else if (engine::renderer::load_texture("texbudget/tiny.bmp") !=
               engine::renderer::kInvalidTextureHandle) {
      // Headless: no device, so the load cannot fully succeed.
      result = 80;
    } else if (captured_log_contains("decode budget")) {
      result = 81;
    }
  }

  engine::core::log_unregister_sink(&capture_log_sink, nullptr);
  engine::core::shutdown_logging();
  engine::renderer::shutdown_texture_system();
  static_cast<void>(engine::core::unmount("texbudget"));
  std::filesystem::remove_all(kDir, ec);
  return result;
}

int main() {
  int result = check_init_shutdown();
  if (result != 0) {
    return result;
  }

  result = check_null_path();
  if (result != 0) {
    return result;
  }

  result = check_invalid_handle();
  if (result != 0) {
    return result;
  }

  result = check_load_before_init();
  if (result != 0) {
    return result;
  }

  result = check_stb_input_size_validation();
  if (result != 0) {
    return result;
  }


  result = check_texture_asset_database();
  if (result != 0) {
    return result;
  }

  result = check_cubemap_invalid_args();
  if (result != 0) {
    return result;
  }

  result = check_decode_budget();
  if (result != 0) {
    return result;
  }

  result = check_skybox_assignment();
  if (result != 0) {
    return result;
  }

  return 0;
}
