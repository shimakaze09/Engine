// Verifies texture handle generation prevents stale slot reuse.

#include "engine/core/vfs.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/texture_loader.h"

#include <cstdint>
#include <cstdio>

namespace engine::renderer {
namespace {

struct FakeTextureDevice final {
  std::uint32_t nextId = 1U;
  int aliveTextures = 0;
};

FakeTextureDevice g_fake{};
RenderDevice g_device{};

std::uint32_t fake_create_texture_2d(std::int32_t, std::int32_t, std::int32_t,
                                     const void *) noexcept {
  ++g_fake.aliveTextures;
  return g_fake.nextId++;
}

void fake_destroy_texture(std::uint32_t id) noexcept {
  if (id != 0U) {
    --g_fake.aliveTextures;
  }
}

void reset_fake_device() noexcept {
  g_fake = FakeTextureDevice{};
  g_device = RenderDevice{};
  g_device.create_texture_2d = &fake_create_texture_2d;
  g_device.destroy_texture = &fake_destroy_texture;
}

} // namespace

const RenderDevice *render_device() noexcept { return &g_device; }

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
  if (engine::renderer::texture_gpu_id(first) != 1U) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 15;
  }

  engine::renderer::unload_texture(first);
  if (engine::renderer::texture_gpu_id(first) != 0U) {
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
  if (engine::renderer::texture_gpu_id(second) != 2U) {
    engine::renderer::shutdown_texture_system();
    engine::core::shutdown_vfs();
    return 18;
  }
  if (engine::renderer::texture_gpu_id(first) != 0U) {
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
  if (engine::renderer::register_external_texture(7U) !=
      engine::renderer::kInvalidTextureHandle) {
    return 30;
  }

  if (!engine::renderer::initialize_texture_system()) {
    return 31;
  }

  const engine::renderer::TextureHandle handle =
      engine::renderer::register_external_texture(77U);
  if (handle == engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    return 32;
  }
  if (engine::renderer::texture_gpu_id(handle) != 77U) {
    engine::renderer::shutdown_texture_system();
    return 33;
  }
  // No GL texture was created on the device for an external registration.
  if (engine::renderer::fake_alive_textures() != 0) {
    engine::renderer::shutdown_texture_system();
    return 34;
  }

  if (!engine::renderer::update_external_texture(handle, 88U) ||
      (engine::renderer::texture_gpu_id(handle) != 88U)) {
    engine::renderer::shutdown_texture_system();
    return 35;
  }

  // Unload releases the slot without touching the GL object (the fake
  // device would go negative if destroy were called).
  engine::renderer::unload_texture(handle);
  if (engine::renderer::texture_gpu_id(handle) != 0U) {
    engine::renderer::shutdown_texture_system();
    return 36;
  }
  if (engine::renderer::fake_alive_textures() != 0) {
    engine::renderer::shutdown_texture_system();
    return 37;
  }

  // A stale handle can no longer be updated.
  if (engine::renderer::update_external_texture(handle, 99U)) {
    engine::renderer::shutdown_texture_system();
    return 38;
  }

  // Shutdown must also skip external GL objects.
  const engine::renderer::TextureHandle survivor =
      engine::renderer::register_external_texture(55U);
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

} // namespace

/// Runs this executable or test program.
int main() {
  const int externalResult = check_external_texture_registration();
  if (externalResult != 0) {
    return externalResult;
  }

  return check_texture_handle_generation();
}
