// Verifies texture handle generation prevents stale slot reuse, that the
// loader serves every slot its handle can name, that a device whose
// texture table is full refuses a texture before the file is read, and
// that a texture's import settings, its own or its folder's, decide how it
// is created.

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/texture_loader.h"
#include "texture_handle_codec.h"

#include "../fake_render_device.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace engine::renderer {

void reset_fake_device() noexcept {
  tests::reset_fake_device();
  tests::fake_device().create_texture = &tests::fake::create_texture;
  tests::fake_device().destroy_texture = &tests::fake::destroy_texture;
}

// When set, render_device() answers as it does after shutdown_render_device:
// no device is live.
void set_fake_device_absent(bool absent) noexcept {
  tests::fake_log().present = !absent;
}

int fake_alive_textures() noexcept {
  return tests::fake_alive(tests::FakeKind::Texture);
}

} // namespace engine::renderer

namespace {

constexpr const char *kTexturePath = "tex/texture_handle_reuse.png";

// 1x1 transparent RGBA PNG.
constexpr unsigned char kTinyPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
    0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89,
    0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
    0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60,
    0x82};

// 1x1 8-bit greyscale PNG, value 128.
constexpr unsigned char kGreyPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x3A, 0x7E, 0x9B, 0x55, 0x00, 0x00, 0x00,
    0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x68, 0x00, 0x00, 0x00,
    0x82, 0x00, 0x81, 0x77, 0xCD, 0x72, 0xB6, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

/// A colour texture is created sRGB, expanded to RGBA8 (the one 8-bit
/// format devices sample as sRGB) whatever its channel count; a data
/// texture keeps its channels and is created linear (issue #810).
int check_color_space_reaches_the_device() {
  using engine::renderer::TextureColorSpace;
  using engine::renderer::TextureFormat;
  constexpr const char *kGreyPath = "tex/texture_color_space_grey.png";
  engine::renderer::reset_fake_device();
  if (!engine::core::initialize_vfs() || !engine::core::mount("tex", ".") ||
      !engine::core::vfs_write_binary(kGreyPath, kGreyPng, sizeof(kGreyPng)) ||
      !engine::renderer::initialize_texture_system()) {
    engine::core::shutdown_vfs();
    return 60;
  }
  int result = 0;
  const engine::renderer::TextureDesc &last =
      engine::tests::fake_log().lastTexture;
  const engine::renderer::TextureHandle colour =
      engine::renderer::load_texture(kGreyPath, TextureColorSpace::Srgb);
  if ((colour == engine::renderer::kInvalidTextureHandle) || !last.srgb ||
      (last.format != TextureFormat::RGBA8)) {
    result = 61;
  }
  const engine::renderer::TextureHandle data =
      engine::renderer::load_texture(kGreyPath, TextureColorSpace::Linear);
  if ((result == 0) && ((data == engine::renderer::kInvalidTextureHandle) ||
                        last.srgb || (last.format != TextureFormat::R8))) {
    result = 62;
  }
  engine::renderer::unload_texture(colour);
  engine::renderer::unload_texture(data);
  engine::renderer::shutdown_texture_system();
  static_cast<void>(std::remove("texture_color_space_grey.png"));
  engine::core::shutdown_vfs();
  return result;
}

/// A texture's import settings, from its sidecar, decide how it is
/// created: an authored Linear colour space wins over the sRGB a colour
/// slot asks for, no mips, nearest filtering and clamp reach the device,
/// and the colour space reads as authored. With no sidecar the texture
/// loads at the defaults. The newer of the image's and the sidecar's
/// write times is the texture's input time, so a settings edit reloads it.
int check_import_settings_reach_the_device() {
  using engine::renderer::TextureColorSpace;
  using engine::renderer::TextureFilter;
  using engine::renderer::TextureFormat;
  using engine::renderer::TextureWrap;
  constexpr const char *kPath = "tex/texture_import_settings.png";
  constexpr const char *kSidecar = "tex/texture_import_settings.png.meta";
  constexpr const char kSettings[] =
      "{\n  \"schemaVersion\": 1,\n  \"guid\": "
      "\"33333333-4444-4555-8666-777777777777\",\n  \"importSettings\": {"
      "\n    \"colorSpace\": \"linear\",\n    \"generateMips\": false,"
      "\n    \"filter\": \"nearest\",\n    \"wrap\": \"clamp\"\n  }\n}\n";
  engine::renderer::reset_fake_device();
  if (!engine::core::initialize_vfs() || !engine::core::mount("tex", ".") ||
      !engine::core::vfs_write_binary(kPath, kGreyPng, sizeof(kGreyPng)) ||
      !engine::core::vfs_write_binary(kSidecar, kSettings,
                                      sizeof(kSettings) - 1U) ||
      !engine::renderer::initialize_texture_system()) {
    engine::core::shutdown_vfs();
    return 80;
  }
  int result = 0;
  const engine::renderer::TextureDesc &last =
      engine::tests::fake_log().lastTexture;
  const engine::renderer::TextureHandle authored =
      engine::renderer::load_texture(kPath, TextureColorSpace::Srgb);
  if ((authored == engine::renderer::kInvalidTextureHandle) || last.srgb ||
      (last.format != TextureFormat::R8) || (last.mipLevels != 1) ||
      (last.filter != TextureFilter::Nearest) ||
      (last.wrap != TextureWrap::ClampEdge) ||
      !engine::renderer::texture_color_space_authored(authored)) {
    result = 81;
  }

  // The sidecar newer than the image is the texture's input time.
  std::error_code ec{};
  const auto imageTime =
      std::filesystem::last_write_time("texture_import_settings.png", ec);
  std::filesystem::last_write_time("texture_import_settings.png.meta",
                                   imageTime + std::chrono::seconds(5), ec);
  if ((result == 0) && (ec ||
                        (engine::renderer::texture_input_write_time(kPath) !=
                         engine::core::vfs_file_mtime(kSidecar)) ||
                        (engine::renderer::texture_input_write_time(kPath) <=
                         engine::core::vfs_file_mtime(kPath)))) {
    result = 82;
  }

  static_cast<void>(std::remove("texture_import_settings.png.meta"));
  const engine::renderer::TextureHandle defaults =
      engine::renderer::load_texture(kPath, TextureColorSpace::Srgb);
  if ((result == 0) &&
      ((defaults == engine::renderer::kInvalidTextureHandle) || !last.srgb ||
       (last.mipLevels != 0) || (last.filter != TextureFilter::LinearMipmap) ||
       (last.wrap != TextureWrap::Repeat) ||
       engine::renderer::texture_color_space_authored(defaults) ||
       (engine::renderer::texture_input_write_time(kPath) !=
        engine::core::vfs_file_mtime(kPath)))) {
    result = 83;
  }
  // An image with no block of its own takes its folder's, and an edit to
  // the folder's sidecar is the texture's input time.
  constexpr const char kFolderSettings[] =
      "{\n  \"schemaVersion\": 1,\n  \"guid\": "
      "\"33333333-4444-4555-8666-777777777778\",\n  \"folder\": true,\n"
      "  \"importSettings\": {\n    \"texture\": {\n      \"filter\": "
      "\"nearest\"\n    }\n  }\n}\n";
  std::filesystem::create_directories("texture_import_folder", ec);
  engine::renderer::TextureHandle inherited =
      engine::renderer::kInvalidTextureHandle;
  if ((result == 0) &&
      (!engine::core::vfs_write_binary("tex/texture_import_folder/grey.png",
                                       kGreyPng, sizeof(kGreyPng)) ||
       !engine::core::vfs_write_binary("tex/texture_import_folder.meta",
                                       kFolderSettings,
                                       sizeof(kFolderSettings) - 1U))) {
    result = 84;
  }
  if (result == 0) {
    inherited = engine::renderer::load_texture(
        "tex/texture_import_folder/grey.png", TextureColorSpace::Srgb);
    if ((inherited == engine::renderer::kInvalidTextureHandle) ||
        (last.filter != TextureFilter::Nearest) ||
        (last.wrap != TextureWrap::Repeat) || (last.mipLevels != 0)) {
      result = 85;
    }
  }
  const auto folderImageTime =
      std::filesystem::last_write_time("texture_import_folder/grey.png", ec);
  std::filesystem::last_write_time("texture_import_folder.meta",
                                   folderImageTime + std::chrono::seconds(5),
                                   ec);
  if ((result == 0) && (ec || (engine::renderer::texture_input_write_time(
                                   "tex/texture_import_folder/grey.png") !=
                               engine::core::vfs_file_mtime(
                                   "tex/texture_import_folder.meta")))) {
    result = 86;
  }

  engine::renderer::unload_texture(authored);
  engine::renderer::unload_texture(defaults);
  engine::renderer::unload_texture(inherited);
  engine::renderer::shutdown_texture_system();
  static_cast<void>(std::remove("texture_import_settings.png"));
  std::filesystem::remove_all("texture_import_folder", ec);
  static_cast<void>(std::remove("texture_import_folder.meta"));
  engine::core::shutdown_vfs();
  return result;
}

int check_texture_handle_generation() {
  engine::renderer::reset_fake_device();
  if (!engine::core::initialize_vfs()) {
    return 10;
  }
  if (!engine::core::mount("tex", ".")) {
    engine::core::shutdown_vfs();
    return 11;
  }
  if (!engine::core::vfs_write_binary(kTexturePath, kTinyPng,
                                      sizeof(kTinyPng))) {
    engine::core::shutdown_vfs();
    return 12;
  }
  if (!engine::renderer::initialize_texture_system()) {
    engine::core::shutdown_vfs();
    return 13;
  }

  const engine::renderer::TextureHandle first =
      engine::renderer::load_texture(kTexturePath);
  if (first == engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 14;
  }
  if (engine::renderer::texture_device_handle(first) !=
      engine::renderer::DeviceTextureHandle{1U}) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 15;
  }

  engine::renderer::unload_texture(first);
  if (engine::renderer::texture_device_handle(first) !=
      engine::renderer::DeviceTextureHandle{0U}) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 16;
  }

  const engine::renderer::TextureHandle second =
      engine::renderer::load_texture(kTexturePath);
  if ((second == engine::renderer::kInvalidTextureHandle) ||
      (second == first)) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 17;
  }
  if (engine::renderer::texture_device_handle(second) !=
      engine::renderer::DeviceTextureHandle{2U}) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 18;
  }
  if (engine::renderer::texture_device_handle(first) !=
      engine::renderer::DeviceTextureHandle{0U}) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 19;
  }

  engine::renderer::unload_texture(first);
  if (engine::renderer::fake_alive_textures() != 1) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 20;
  }

  engine::renderer::unload_texture(second);
  if (engine::renderer::fake_alive_textures() != 0) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 21;
  }

  engine::renderer::shutdown_texture_system();
  engine::core::shutdown_vfs();
  static_cast<void>(std::remove("texture_handle_reuse.png"));
  return 0;
}

// External registrations alias GL textures owned elsewhere: the texture
// system must never destroy them, on unload or on shutdown.
int check_external_texture_registration() {
  engine::renderer::reset_fake_device();

  // Registration requires an initialized texture system.
  if (engine::renderer::register_external_texture(
      engine::renderer::DeviceTextureHandle{7U}) !=
      engine::renderer::kInvalidTextureHandle) {
    return 30;
  }

  if (!engine::renderer::initialize_texture_system()) {
    return 31;
  }

  const engine::renderer::TextureHandle handle =
      engine::renderer::register_external_texture(
      engine::renderer::DeviceTextureHandle{77U});
  if (handle == engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    return 32;
  }
  if (engine::renderer::texture_device_handle(handle) !=
      engine::renderer::DeviceTextureHandle{77U}) {
    engine::renderer::shutdown_texture_system();
    return 33;
  }
  // No device texture was created for an external registration.
  if (engine::renderer::fake_alive_textures() != 0) {
    engine::renderer::shutdown_texture_system();
    return 34;
  }

  if (!engine::renderer::update_external_texture(
      handle, engine::renderer::DeviceTextureHandle{88U}) ||
      (engine::renderer::texture_device_handle(handle) !=
      engine::renderer::DeviceTextureHandle{88U})) {
    engine::renderer::shutdown_texture_system();
    return 35;
  }

  // Unload releases the slot without touching the GL object (the fake
  // device would go negative if destroy were called).
  engine::renderer::unload_texture(handle);
  if (engine::renderer::texture_device_handle(handle) !=
      engine::renderer::DeviceTextureHandle{0U}) {
    engine::renderer::shutdown_texture_system();
    return 36;
  }
  if (engine::renderer::fake_alive_textures() != 0) {
    engine::renderer::shutdown_texture_system();
    return 37;
  }

  // A stale handle can no longer be updated.
  if (engine::renderer::update_external_texture(
      handle, engine::renderer::DeviceTextureHandle{99U})) {
    engine::renderer::shutdown_texture_system();
    return 38;
  }

  const engine::renderer::TextureHandle survivor =
      engine::renderer::register_external_texture(
      engine::renderer::DeviceTextureHandle{55U});
  if (survivor == engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    return 39;
  }
  engine::renderer::shutdown_texture_system();
  if (engine::renderer::fake_alive_textures() != 0) {
    return 40;
  }

  return 0;
}


/// Counts registry-closed-after-device warnings reaching the log.
int g_unreleasedWarnings = 0;

void count_unreleased_warning(engine::core::LogLevel level, const char *,
                              const char *message, void *) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (message != nullptr) &&
      (std::strstr(message, "texture registry closed after the render "
                            "device") != nullptr)) {
    ++g_unreleasedWarnings;
  }
}

/// A registry closed while the device is live releases every owned texture
/// silently; one closed after the device is gone can release nothing and
/// must say so, once, with the count. External aliases never count either
/// way.
int check_shutdown_reports_textures_it_cannot_release() {
  engine::renderer::reset_fake_device();
  g_unreleasedWarnings = 0;
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_unreleased_warning, nullptr)) {
    return 60;
  }
  int result = 0;
  if (!engine::core::initialize_vfs() || !engine::core::mount("tex", ".") ||
      !engine::core::vfs_write_binary(kTexturePath, kTinyPng,
                                      sizeof(kTinyPng))) {
    result = 61;
  }

  // Device live: two owned textures and one external alias close quietly.
  if ((result == 0) && engine::renderer::initialize_texture_system()) {
    const bool loaded =
        (engine::renderer::load_texture(kTexturePath) !=
         engine::renderer::kInvalidTextureHandle) &&
        (engine::renderer::load_texture(kTexturePath) !=
         engine::renderer::kInvalidTextureHandle) &&
        (engine::renderer::register_external_texture(
             engine::renderer::DeviceTextureHandle{77U}) !=
         engine::renderer::kInvalidTextureHandle);
    engine::renderer::shutdown_texture_system();
    if (!loaded) {
      result = 62;
    } else if (engine::renderer::fake_alive_textures() != 0) {
      result = 63;
    } else if (g_unreleasedWarnings != 0) {
      result = 64;
    }
  } else if (result == 0) {
    result = 65;
  }

  // Device gone first: the same registry content cannot be released, and
  // the close reports exactly the owned textures (the alias excluded).
  if ((result == 0) && engine::renderer::initialize_texture_system()) {
    const bool loaded =
        (engine::renderer::load_texture(kTexturePath) !=
         engine::renderer::kInvalidTextureHandle) &&
        (engine::renderer::load_texture(kTexturePath) !=
         engine::renderer::kInvalidTextureHandle) &&
        (engine::renderer::register_external_texture(
             engine::renderer::DeviceTextureHandle{77U}) !=
         engine::renderer::kInvalidTextureHandle);
    engine::renderer::set_fake_device_absent(true);
    engine::renderer::shutdown_texture_system();
    engine::renderer::set_fake_device_absent(false);
    if (!loaded) {
      result = 66;
    } else if (engine::renderer::fake_alive_textures() != 2) {
      result = 67;
    } else if (g_unreleasedWarnings != 1) {
      result = 68;
    } else if (engine::renderer::register_external_texture(
                   engine::renderer::DeviceTextureHandle{5U}) !=
               engine::renderer::kInvalidTextureHandle) {
      // The registry is closed regardless of what it could release.
      result = 69;
    }
  } else if (result == 0) {
    result = 70;
  }

  // An empty registry has nothing to report even without a device.
  if ((result == 0) && engine::renderer::initialize_texture_system()) {
    engine::renderer::set_fake_device_absent(true);
    engine::renderer::shutdown_texture_system();
    engine::renderer::set_fake_device_absent(false);
    if (g_unreleasedWarnings != 1) {
      result = 71;
    }
  } else if (result == 0) {
    result = 72;
  }

  engine::core::shutdown_vfs();
  engine::core::log_unregister_sink(&count_unreleased_warning, nullptr);
  static_cast<void>(std::remove("texture_handle_reuse.png"));
  return result;
}

/// Counts the full-table refusals reaching the log, and whether each named
/// the device's capacity.
int g_fullTableErrors = 0;
bool g_fullTableNamedCap = true;

void count_full_table_error(engine::core::LogLevel level, const char *,
                            const char *message, void *) noexcept {
  if ((level == engine::core::LogLevel::Error) && (message != nullptr) &&
      (std::strstr(message, "texture table is full") != nullptr)) {
    ++g_fullTableErrors;
    g_fullTableNamedCap = g_fullTableNamedCap &&
                          (std::strstr(message, "(2 textures)") != nullptr);
  }
}

/// A device whose texture table is full refuses the next texture with one
/// diagnostic naming its capacity, before the loader reads or decodes the
/// file, so nothing reaches create_texture; a freed slot takes a texture
/// again (issue #925).
int check_full_device_table_refuses_before_decode() {
  engine::renderer::reset_fake_device();
  engine::tests::fake_device().caps.maxTextures = 2U;
  engine::tests::fake_device().texture_slots_free =
      &engine::tests::fake::texture_slots_free;
  engine::tests::fake_log().textureCapacity = 2U;
  g_fullTableErrors = 0;
  g_fullTableNamedCap = true;
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_full_table_error, nullptr)) {
    return 80;
  }
  int result = 0;
  if (!engine::core::initialize_vfs() || !engine::core::mount("tex", ".") ||
      !engine::core::vfs_write_binary(kTexturePath, kTinyPng,
                                      sizeof(kTinyPng)) ||
      !engine::renderer::initialize_texture_system()) {
    result = 81;
  }
  using engine::renderer::kInvalidTextureHandle;
  using engine::renderer::load_texture;
  const auto creates = [] {
    return engine::tests::fake_creates(engine::tests::FakeKind::Texture);
  };
  engine::renderer::TextureHandle first = kInvalidTextureHandle;
  if (result == 0) {
    first = load_texture(kTexturePath);
    const engine::renderer::TextureHandle second = load_texture(kTexturePath);
    if ((first == kInvalidTextureHandle) || (second == kInvalidTextureHandle) ||
        (creates() != 2) || (g_fullTableErrors != 0)) {
      result = 82;
    }
  }
  if ((result == 0) && ((load_texture(kTexturePath) != kInvalidTextureHandle) ||
                        (creates() != 2))) {
    result = 83; // loaded, or reached the device, past the table
  }
  if ((result == 0) && ((g_fullTableErrors != 1) || !g_fullTableNamedCap)) {
    result = 84; // not exactly one diagnostic naming the capacity
  }
  if (result == 0) {
    engine::renderer::unload_texture(first);
    if ((load_texture(kTexturePath) == kInvalidTextureHandle) ||
        (creates() != 3) || (g_fullTableErrors != 1)) {
      result = 85; // a freed slot does not take a texture again
    }
  }
  engine::renderer::shutdown_texture_system();
  engine::core::shutdown_vfs();
  engine::core::log_unregister_sink(&count_full_table_error, nullptr);
  static_cast<void>(std::remove("texture_handle_reuse.png"));
  return result;
}

/// Encoded generations wrap inside their 22-bit field without becoming zero.
int check_texture_generation_wrap() noexcept {
  namespace codec = engine::renderer::texture_handle_detail;
  if (codec::next_generation(codec::kGenerationMask - 1U) !=
          codec::kGenerationMask ||
      (codec::next_generation(codec::kGenerationMask) != 1U)) {
    return 50;
  }

  constexpr engine::renderer::TextureHandle maximum =
      codec::make_handle(511U, codec::kGenerationMask);
  constexpr engine::renderer::TextureHandle wrapped =
      codec::make_handle(511U, codec::next_generation(codec::kGenerationMask));
  if ((maximum == engine::renderer::kInvalidTextureHandle) ||
      (wrapped == engine::renderer::kInvalidTextureHandle) ||
      (maximum == wrapped) || (codec::slot_index(maximum) != 511U) ||
      (codec::generation(maximum) != codec::kGenerationMask) ||
      (codec::generation(wrapped) != 1U)) {
    return 51;
  }
  return 0;
}


} // namespace

/// Runs this executable or test program.
/// The loader serves every slot its handle's slot field can name (#663):
/// asset_database.cpp checks that is enough for the whole material texture
/// table and as many more, so here every slot registers to a distinct live
/// handle and one past the last is refused.
int check_loader_fills_every_slot() {
  namespace codec = engine::renderer::texture_handle_detail;
  engine::renderer::reset_fake_device();
  if (!engine::renderer::initialize_texture_system()) {
    return 60;
  }
  std::size_t registered = 0U;
  bool resolves = true;
  for (std::uint32_t i = 0U; i <= codec::kSlotMask; ++i) {
    const engine::renderer::DeviceTextureHandle device{100U + i};
    const engine::renderer::TextureHandle handle =
        engine::renderer::register_external_texture(device);
    if (handle == engine::renderer::kInvalidTextureHandle) {
      break;
    }
    resolves =
        resolves && (engine::renderer::texture_device_handle(handle) == device);
    ++registered;
  }
  engine::renderer::shutdown_texture_system();
  if ((registered != codec::kSlotMask) || !resolves) {
    std::printf("the loader held %zu of %u textures\n", registered,
                static_cast<unsigned>(codec::kSlotMask));
    return 61;
  }
  return 0;
}

int main() {
  const int wrapResult = check_texture_generation_wrap();
  if (wrapResult != 0) {
    return wrapResult;
  }

  const int externalResult = check_external_texture_registration();
  if (externalResult != 0) {
    return externalResult;
  }

  const int unreleasedResult = check_shutdown_reports_textures_it_cannot_release();
  if (unreleasedResult != 0) {
    return unreleasedResult;
  }

  const int fillResult = check_loader_fills_every_slot();
  if (fillResult != 0) {
    return fillResult;
  }

  const int fullResult = check_full_device_table_refuses_before_decode();
  if (fullResult != 0) {
    return fullResult;
  }

  const int colourResult = check_color_space_reaches_the_device();
  if (colourResult != 0) {
    return colourResult;
  }

  const int settingsResult = check_import_settings_reach_the_device();
  if (settingsResult != 0) {
    return settingsResult;
  }

  return check_texture_handle_generation();
}
