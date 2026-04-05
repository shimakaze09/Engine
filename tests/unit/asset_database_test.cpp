#include <memory>
#include <new>

#include "engine/renderer/asset_database.h"

int main() {
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    return 100;
  }

  engine::renderer::clear_asset_database(database.get());

  constexpr engine::renderer::AssetId kAssetId = 77U;
  constexpr engine::renderer::MeshHandle kMeshHandle{5U};

  if (!engine::renderer::register_mesh_asset(
          database.get(), kAssetId, "assets/test.mesh", kMeshHandle)) {
    return 1;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId)
      != engine::renderer::AssetState::Ready) {
    return 2;
  }

  if (!engine::renderer::mesh_asset_requested_resident(database.get(),
                                                       kAssetId)) {
    return 3;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId)
      != kMeshHandle) {
    return 4;
  }

  if (!engine::renderer::retain_mesh_asset(database.get(), kAssetId)) {
    return 5;
  }

  if (!engine::renderer::release_mesh_asset(database.get(), kAssetId)) {
    return 6;
  }

  if (!engine::renderer::release_mesh_asset(database.get(), kAssetId)) {
    return 7;
  }

  if (engine::renderer::mesh_asset_requested_resident(database.get(),
                                                      kAssetId)) {
    return 8;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId)
      != kMeshHandle) {
    return 9;
  }

  if (!engine::renderer::set_mesh_asset_state(
          database.get(),
          kAssetId,
          engine::renderer::AssetState::Loading,
          engine::renderer::kInvalidMeshHandle)) {
    return 10;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId)
      != engine::renderer::AssetState::Loading) {
    return 11;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId)
      != engine::renderer::kInvalidMeshHandle) {
    return 12;
  }

  if (!engine::renderer::set_mesh_asset_state(
          database.get(),
          kAssetId,
          engine::renderer::AssetState::Failed,
          engine::renderer::kInvalidMeshHandle)) {
    return 13;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId)
      != engine::renderer::kInvalidMeshHandle) {
    return 14;
  }

  if (engine::renderer::set_mesh_asset_state(
          database.get(),
          kAssetId,
          engine::renderer::AssetState::Ready,
          engine::renderer::kInvalidMeshHandle)) {
    return 15;
  }

  if (!engine::renderer::set_mesh_asset_state(
          database.get(),
          kAssetId,
          engine::renderer::AssetState::Ready,
          kMeshHandle)) {
    return 16;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId)
      != kMeshHandle) {
    return 17;
  }

  return 0;
}
