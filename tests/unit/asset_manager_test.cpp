#include <memory>
#include <new>

#include "engine/renderer/asset_manager.h"

namespace {

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

  constexpr engine::renderer::AssetId kAssetId = 101U;
  if (!engine::renderer::queue_mesh_load(
          manager.get(), database.get(), kAssetId, "assets/missing.mesh")) {
    return 2;
  }

  if (engine::renderer::pending_asset_request_count(manager.get()) == 0U) {
    return 3;
  }

  if (engine::renderer::update_asset_manager(
          manager.get(), database.get(), registry.get(), 4U)) {
    return 4;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId)
      != engine::renderer::AssetState::Failed) {
    return 5;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId)
      != engine::renderer::kInvalidMeshHandle) {
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

  constexpr engine::renderer::AssetId kAssetId = 102U;
  if (!engine::renderer::queue_mesh_load(
          manager.get(), database.get(), kAssetId, "assets/missing.mesh")) {
    return 21;
  }

  if (!engine::renderer::queue_mesh_unload(
          manager.get(), database.get(), kAssetId)) {
    return 22;
  }

  static_cast<void>(engine::renderer::update_asset_manager(
      manager.get(), database.get(), registry.get(), 8U));

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId)
      != engine::renderer::AssetState::Unloaded) {
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

  constexpr engine::renderer::AssetId kAssetId = 103U;
  constexpr engine::renderer::MeshHandle kMeshHandle{3U};
  registry->occupied[kMeshHandle.id] = true;
  registry->meshes[kMeshHandle.id] = engine::renderer::GpuMesh{};

  if (!engine::renderer::register_mesh_asset(
          database.get(), kAssetId, "assets/test.mesh", kMeshHandle)) {
    return 41;
  }

  if (!engine::renderer::queue_mesh_unload(
          manager.get(), database.get(), kAssetId)) {
    return 42;
  }

  if (!engine::renderer::update_asset_manager(
          manager.get(), database.get(), registry.get(), 4U)) {
    return 43;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId)
      != engine::renderer::AssetState::Unloaded) {
    return 44;
  }

  if (registry->occupied[kMeshHandle.id]) {
    return 45;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId)
      != engine::renderer::kInvalidMeshHandle) {
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

  constexpr engine::renderer::AssetId kAssetId = 104U;
  constexpr engine::renderer::MeshHandle kMeshHandle{4U};
  registry->occupied[kMeshHandle.id] = true;
  registry->meshes[kMeshHandle.id] = engine::renderer::GpuMesh{};

  if (!engine::renderer::register_mesh_asset(
          database.get(), kAssetId, "assets/test.mesh", kMeshHandle)) {
    return 61;
  }

  if (!engine::renderer::release_mesh_asset(database.get(), kAssetId)) {
    return 62;
  }

  if (!engine::renderer::update_asset_manager(
          manager.get(), database.get(), registry.get(), 4U)) {
    return 63;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId)
      != engine::renderer::AssetState::Unloaded) {
    return 64;
  }

  if (registry->occupied[kMeshHandle.id]) {
    return 65;
  }

  return 0;
}

} // namespace

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

  return verify_auto_unload_from_release_intent();
}
