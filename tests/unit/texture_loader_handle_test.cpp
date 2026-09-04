// Verifies texture handle generation prevents stale slot reuse.

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/texture_loader.h"
#include "texture_handle_codec.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::renderer {
namespace {

struct FakeTextureDevice final {
  std::uint32_t nextId = 1U;
  int aliveTextures = 0;
};

FakeTextureDevice g_fake{};
RenderDevice g_device{};
// When set, render_device() answers as it does after shutdown_render_device:
// no device is live.
bool g_deviceAbsent = false;

DeviceTextureHandle fake_create_texture(const TextureDesc &) noexcept {
  ++g_fake.aliveTextures;
  return DeviceTextureHandle{g_fake.nextId++};
}

void fake_destroy_texture(DeviceTextureHandle texture) noexcept {
  if (texture.value != 0U) {
    --g_fake.aliveTextures;
  }
}

void reset_fake_device() noexcept {
  g_fake = FakeTextureDevice{};
  g_device = RenderDevice{};
  g_device.create_texture = &fake_create_texture;
  g_device.destroy_texture = &fake_destroy_texture;
  g_deviceAbsent = false;
}

} // namespace

const RenderDevice *render_device() noexcept {
  return g_deviceAbsent ? nullptr : &g_device;
}

void set_fake_device_absent(bool absent) noexcept { g_deviceAbsent = absent; }

int fake_alive_textures() noexcept { return g_fake.aliveTextures; }

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

  return check_texture_handle_generation();
}
