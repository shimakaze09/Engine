// Verifies asset manager test behavior for the Engine test suite.

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/core/mesh_asset.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

namespace {

/// Writes a minimal valid v1 cooked mesh (one triangle) so reload paths can
/// decode a real replacement; `yOffset` varies the bytes between fixtures.
bool write_v1_mesh_file(const char *path, float yOffset) noexcept {
  std::FILE *file = nullptr;
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

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 3U;
  header.indexCount = 3U;
  // v1 stride: position3 + normal3 per vertex.
  const std::array<float, 18U> vertices = {
      0.0F, yOffset,        0.0F, 0.0F, 1.0F, 0.0F,
      1.0F, yOffset,        0.0F, 0.0F, 1.0F, 0.0F,
      0.0F, yOffset + 1.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  const std::array<std::uint32_t, 3U> indices = {0U, 1U, 2U};

  bool ok = std::fwrite(&header, sizeof(header), 1U, file) == 1U;
  ok = ok && (std::fwrite(vertices.data(), sizeof(float), vertices.size(),
                          file) == vertices.size());
  ok = ok && (std::fwrite(indices.data(), sizeof(std::uint32_t),
                          indices.size(), file) == indices.size());
  return (std::fclose(file) == 0) && ok;
}

/// Writes garbage bytes to the path so a reload's decode fails.
bool write_garbage_file(const char *path) noexcept {
  std::FILE *file = nullptr;
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
  const char garbage[] = "not a mesh";
  const bool ok =
      std::fwrite(garbage, 1U, sizeof(garbage), file) == sizeof(garbage);
  return (std::fclose(file) == 0) && ok;
}

/// Regression for issue #388: a forced reload must prove the replacement
/// before touching the record's live mesh. A reload whose decode fails
/// (missing file, malformed bytes) or whose registration fails (registry
/// full) keeps the previous Ready mesh, handle, and state; a successful
/// reload swaps once, retiring the old handle exactly once; and a
/// released record still unloads normally afterward. Runs under an open
/// renderer lifetime so decode+upload work headlessly on the Noop device.
int verify_reload_stages_replacement() {
  std::unique_ptr<engine::renderer::AssetManager> manager(
      new (std::nothrow) engine::renderer::AssetManager());
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> registry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  if ((manager == nullptr) || (database == nullptr) || (registry == nullptr)) {
    return 80;
  }

  engine::renderer::clear_asset_manager(manager.get());
  engine::renderer::clear_asset_database(database.get());

  engine::renderer::initialize_renderer();
  if (!engine::renderer::initialize_render_device()) {
    return 81;
  }

  constexpr const char *kGoodPath = "am_reload_good.mesh";
  constexpr const char *kMissingPath = "am_reload_missing.mesh";
  constexpr const char *kGarbagePath = "am_reload_garbage.mesh";
  constexpr engine::renderer::AssetId kAssetId = 105ULL;
  int failure = 0;

  if (!write_v1_mesh_file(kGoodPath, 0.0F) ||
      !write_garbage_file(kGarbagePath)) {
    failure = 82;
  }

  // Baseline: a real load through the production queue reaches Ready.
  if (failure == 0) {
    if (!engine::renderer::queue_mesh_load(manager.get(), database.get(),
                                           kAssetId, kGoodPath) ||
        !engine::renderer::update_asset_manager(manager.get(), database.get(),
                                                registry.get(), 4U)) {
      failure = 83;
    }
  }
  const engine::renderer::MeshHandle originalHandle =
      engine::renderer::resolve_mesh_asset(database.get(), kAssetId);
  if ((failure == 0) &&
      ((engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
        engine::renderer::AssetState::Ready) ||
       (originalHandle == engine::renderer::kInvalidMeshHandle))) {
    failure = 84;
  }

  // Missing replacement: the update reports failure and the record keeps
  // the previous mesh untouched.
  if (failure == 0) {
    if (!engine::renderer::queue_mesh_reload(manager.get(), database.get(),
                                             kAssetId, kMissingPath) ||
        engine::renderer::update_asset_manager(manager.get(), database.get(),
                                               registry.get(), 4U)) {
      failure = 85;
    } else if ((engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
                engine::renderer::AssetState::Ready) ||
               (engine::renderer::resolve_mesh_asset(database.get(),
                                                     kAssetId) !=
                originalHandle) ||
               (engine::renderer::lookup_gpu_mesh(registry.get(),
                                                  originalHandle) ==
                nullptr)) {
      failure = 86;
    }
  }

  // Malformed replacement bytes: same preservation.
  if (failure == 0) {
    if (!engine::renderer::queue_mesh_reload(manager.get(), database.get(),
                                             kAssetId, kGarbagePath) ||
        engine::renderer::update_asset_manager(manager.get(), database.get(),
                                               registry.get(), 4U)) {
      failure = 87;
    } else if ((engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
                engine::renderer::AssetState::Ready) ||
               (engine::renderer::resolve_mesh_asset(database.get(),
                                                     kAssetId) !=
                originalHandle)) {
      failure = 88;
    }
  }

  // Registry capacity: with every remaining slot occupied the replacement
  // cannot register, and the old mesh still survives.
  std::size_t fillerCount = 0U;
  std::array<engine::renderer::MeshHandle,
             engine::renderer::GpuMeshRegistry::kMaxSlots>
      fillers{};
  if (failure == 0) {
    for (;;) {
      const engine::renderer::MeshHandle filler =
          engine::renderer::register_gpu_mesh(registry.get(),
                                              engine::renderer::GpuMesh{});
      if (filler == engine::renderer::kInvalidMeshHandle) {
        break;
      }
      fillers[fillerCount] = filler;
      ++fillerCount;
    }
    if (!engine::renderer::queue_mesh_reload(manager.get(), database.get(),
                                             kAssetId, kGoodPath) ||
        engine::renderer::update_asset_manager(manager.get(), database.get(),
                                               registry.get(), 4U)) {
      failure = 89;
    } else if ((engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
                engine::renderer::AssetState::Ready) ||
               (engine::renderer::resolve_mesh_asset(database.get(),
                                                     kAssetId) !=
                originalHandle)) {
      failure = 90;
    }
    if (fillerCount > 0U) {
      engine::renderer::unload_gpu_mesh(registry.get(),
                                        fillers[fillerCount - 1U]);
      --fillerCount;
    }
  }

  // Successful reload: the record swaps to the replacement and the old
  // handle is retired exactly once (its slot lookup goes stale).
  engine::renderer::MeshHandle swappedHandle =
      engine::renderer::kInvalidMeshHandle;
  if (failure == 0) {
    if (!write_v1_mesh_file(kGoodPath, 5.0F) ||
        !engine::renderer::queue_mesh_reload(manager.get(), database.get(),
                                             kAssetId, kGoodPath) ||
        !engine::renderer::update_asset_manager(manager.get(), database.get(),
                                                registry.get(), 4U)) {
      failure = 91;
    } else {
      swappedHandle =
          engine::renderer::resolve_mesh_asset(database.get(), kAssetId);
      if ((engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
           engine::renderer::AssetState::Ready) ||
          (swappedHandle == engine::renderer::kInvalidMeshHandle) ||
          (swappedHandle == originalHandle) ||
          (engine::renderer::lookup_gpu_mesh(registry.get(), originalHandle) !=
           nullptr) ||
          (engine::renderer::lookup_gpu_mesh(registry.get(), swappedHandle) ==
           nullptr)) {
        failure = 92;
      }
    }
  }

  // Repeated reload: a second staged swap chains from the first.
  if (failure == 0) {
    if (!engine::renderer::queue_mesh_reload(manager.get(), database.get(),
                                             kAssetId, kGoodPath) ||
        !engine::renderer::update_asset_manager(manager.get(), database.get(),
                                                registry.get(), 4U)) {
      failure = 93;
    } else if (engine::renderer::lookup_gpu_mesh(registry.get(),
                                                 swappedHandle) != nullptr) {
      failure = 94;
    }
  }

  // Release interaction: a released record still unloads normally after
  // the staged reloads.
  if (failure == 0) {
    const engine::renderer::MeshHandle lastHandle =
        engine::renderer::resolve_mesh_asset(database.get(), kAssetId);
    if (!engine::renderer::release_mesh_asset(database.get(), kAssetId) ||
        !engine::renderer::update_asset_manager(manager.get(), database.get(),
                                                registry.get(), 4U)) {
      failure = 95;
    } else if ((engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
                engine::renderer::AssetState::Unloaded) ||
               (engine::renderer::lookup_gpu_mesh(registry.get(),
                                                  lastHandle) != nullptr)) {
      failure = 96;
    }
  }

  for (std::size_t i = 0U; i < fillerCount; ++i) {
    engine::renderer::unload_gpu_mesh(registry.get(), fillers[i]);
  }
  static_cast<void>(std::remove(kGoodPath));
  static_cast<void>(std::remove(kGarbagePath));
  engine::renderer::shutdown_renderer();
  return failure;
}

int verify_failed_load_sets_failed_state() {
  std::unique_ptr<engine::renderer::AssetManager> manager(
      new (std::nothrow) engine::renderer::AssetManager());
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> registry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  if ((manager == nullptr) || (database == nullptr) || (registry == nullptr)) {
    return 1;
  }

  engine::renderer::clear_asset_manager(manager.get());
  engine::renderer::clear_asset_database(database.get());

  constexpr engine::renderer::AssetId kAssetId = 101ULL;
  if (!engine::renderer::queue_mesh_load(manager.get(), database.get(),
                                         kAssetId, "assets/missing.mesh")) {
    return 2;
  }

  if (engine::renderer::pending_asset_request_count(manager.get()) == 0U) {
    return 3;
  }

  if (engine::renderer::update_asset_manager(manager.get(), database.get(),
                                             registry.get(), 4U)) {
    return 4;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
      engine::renderer::AssetState::Failed) {
    return 5;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId) !=
      engine::renderer::kInvalidMeshHandle) {
    return 6;
  }

  return 0;
}

int verify_release_during_pending_load_unloads() {
  std::unique_ptr<engine::renderer::AssetManager> manager(
      new (std::nothrow) engine::renderer::AssetManager());
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> registry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  if ((manager == nullptr) || (database == nullptr) || (registry == nullptr)) {
    return 20;
  }

  engine::renderer::clear_asset_manager(manager.get());
  engine::renderer::clear_asset_database(database.get());

  constexpr engine::renderer::AssetId kAssetId = 102ULL;
  if (!engine::renderer::queue_mesh_load(manager.get(), database.get(),
                                         kAssetId, "assets/missing.mesh")) {
    return 21;
  }

  if (!engine::renderer::queue_mesh_unload(manager.get(), database.get(),
                                           kAssetId)) {
    return 22;
  }

  static_cast<void>(engine::renderer::update_asset_manager(
      manager.get(), database.get(), registry.get(), 8U));

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
      engine::renderer::AssetState::Unloaded) {
    return 23;
  }

  return 0;
}

int verify_unload_clears_registry_slot() {
  std::unique_ptr<engine::renderer::AssetManager> manager(
      new (std::nothrow) engine::renderer::AssetManager());
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> registry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  if ((manager == nullptr) || (database == nullptr) || (registry == nullptr)) {
    return 40;
  }

  engine::renderer::clear_asset_manager(manager.get());
  engine::renderer::clear_asset_database(database.get());

  constexpr engine::renderer::AssetId kAssetId = 103ULL;
  const engine::renderer::MeshHandle kMeshHandle =
      engine::renderer::register_gpu_mesh(registry.get(),
                                          engine::renderer::GpuMesh{});
  if (kMeshHandle == engine::renderer::kInvalidMeshHandle) {
    return 40;
  }

  if (!engine::renderer::register_mesh_asset(database.get(), kAssetId,
                                             "assets/test.mesh", kMeshHandle)) {
    return 41;
  }

  if (!engine::renderer::queue_mesh_unload(manager.get(), database.get(),
                                           kAssetId)) {
    return 42;
  }

  if (!engine::renderer::update_asset_manager(manager.get(), database.get(),
                                              registry.get(), 4U)) {
    return 43;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
      engine::renderer::AssetState::Unloaded) {
    return 44;
  }

  // The stale handle must fail lookup once its slot is released, not just
  // read back the raw occupied flag (audit #173).
  if (engine::renderer::lookup_gpu_mesh(registry.get(), kMeshHandle) !=
      nullptr) {
    return 45;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId) !=
      engine::renderer::kInvalidMeshHandle) {
    return 46;
  }

  return 0;
}

int verify_auto_unload_from_release_intent() {
  std::unique_ptr<engine::renderer::AssetManager> manager(
      new (std::nothrow) engine::renderer::AssetManager());
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> registry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  if ((manager == nullptr) || (database == nullptr) || (registry == nullptr)) {
    return 60;
  }

  engine::renderer::clear_asset_manager(manager.get());
  engine::renderer::clear_asset_database(database.get());

  constexpr engine::renderer::AssetId kAssetId = 104ULL;
  const engine::renderer::MeshHandle kMeshHandle =
      engine::renderer::register_gpu_mesh(registry.get(),
                                          engine::renderer::GpuMesh{});
  if (kMeshHandle == engine::renderer::kInvalidMeshHandle) {
    return 60;
  }

  if (!engine::renderer::register_mesh_asset(database.get(), kAssetId,
                                             "assets/test.mesh", kMeshHandle)) {
    return 61;
  }

  if (!engine::renderer::release_mesh_asset(database.get(), kAssetId)) {
    return 62;
  }

  if (!engine::renderer::update_asset_manager(manager.get(), database.get(),
                                              registry.get(), 4U)) {
    return 63;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
      engine::renderer::AssetState::Unloaded) {
    return 64;
  }

  if (engine::renderer::lookup_gpu_mesh(registry.get(), kMeshHandle) !=
      nullptr) {
    return 65;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = verify_failed_load_sets_failed_state();
  if (result != 0) {
    return result;
  }

  result = verify_release_during_pending_load_unloads();
  if (result != 0) {
    return result;
  }

  result = verify_unload_clears_registry_slot();
  if (result != 0) {
    return result;
  }

  result = verify_auto_unload_from_release_intent();
  if (result != 0) {
    return result;
  }

  return verify_reload_stages_replacement();
}
