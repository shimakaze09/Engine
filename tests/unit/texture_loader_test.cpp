#include <cstdio>

#include "engine/renderer/asset_database.h"
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

  const std::uint32_t gpuId =
      engine::renderer::texture_gpu_id(engine::renderer::kInvalidTextureHandle);
  if (gpuId != 0U) {
    engine::renderer::shutdown_texture_system();
    return 32;
  }

  const bool hdr =
      engine::renderer::is_texture_hdr(engine::renderer::kInvalidTextureHandle);
  if (hdr) {
    engine::renderer::shutdown_texture_system();
    return 33;
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

} // namespace

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

  result = check_texture_asset_database();
  if (result != 0) {
    return result;
  }

  return 0;
}
