// Verifies texture loader test behavior for the Engine test suite.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "engine/renderer/asset_database.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/texture_loader.h"
#include "gl_texture_upload_layout.h"

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

/// Verifies exact formats and scoped row alignment for GL texture uploads.
int check_gl_texture_upload_layout() {
  using engine::renderer::detail::GlTextureUploadLayout;
  using engine::renderer::detail::describe_gl_texture_upload;
  using engine::renderer::detail::with_gl_unpack_alignment;

  struct LayoutCase final {
    std::int32_t channels;
    bool hdr;
    std::uint32_t externalFormat;
    std::int32_t internalFormat;
    std::int32_t unpackAlignment;
  };
  constexpr std::array<LayoutCase, 8U> kCases = {{
      {1, false, 0x1903U, 0x1903, 1},
      {2, false, 0x8227U, 0x8227, 2},
      {3, false, 0x1907U, 0x1907, 1},
      {4, false, 0x1908U, 0x1908, 4},
      {1, true, 0x1903U, 0x822D, 4},
      {2, true, 0x8227U, 0x822F, 8},
      {3, true, 0x1907U, 0x881B, 4},
      {4, true, 0x1908U, 0x881A, 8},
  }};

  for (const LayoutCase &testCase : kCases) {
    GlTextureUploadLayout layout{};
    const std::int32_t bytesPerChannel = testCase.hdr ? 4 : 1;
    if (!describe_gl_texture_upload(1, testCase.channels, bytesPerChannel,
                                    testCase.hdr, &layout) ||
        (layout.externalFormat != testCase.externalFormat) ||
        (layout.internalFormat != testCase.internalFormat) ||
        (layout.unpackAlignment != testCase.unpackAlignment)) {
      return 56;
    }
  }

  GlTextureUploadLayout invalid{99U, 99, 8};
  if (describe_gl_texture_upload(1, 5, 1, false, &invalid) ||
      (invalid.externalFormat != 0U) || (invalid.internalFormat != 0) ||
      (invalid.unpackAlignment != 1)) {
    return 57;
  }

  std::int32_t currentAlignment = 8;
  std::array<std::int32_t, 2U> changes{};
  std::size_t changeCount = 0U;
  bool uploadedWithRequiredAlignment = false;
  with_gl_unpack_alignment(
      1,
      [&](std::int32_t *outAlignment) noexcept {
        *outAlignment = currentAlignment;
      },
      [&](std::int32_t alignment) noexcept {
        changes[changeCount++] = alignment;
        currentAlignment = alignment;
      },
      [&]() noexcept { uploadedWithRequiredAlignment = currentAlignment == 1; });
  if (!uploadedWithRequiredAlignment || (changeCount != 2U) ||
      (changes[0] != 1) || (changes[1] != 8) || (currentAlignment != 8)) {
    return 58;
  }

  changeCount = 0U;
  currentAlignment = 1;
  with_gl_unpack_alignment(
      1,
      [&](std::int32_t *outAlignment) noexcept {
        *outAlignment = currentAlignment;
      },
      [&](std::int32_t) noexcept { ++changeCount; }, []() noexcept {});
  return (changeCount == 0U) ? 0 : 59;
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

  result = check_gl_texture_upload_layout();
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

  result = check_skybox_assignment();
  if (result != 0) {
    return result;
  }

  return 0;
}
