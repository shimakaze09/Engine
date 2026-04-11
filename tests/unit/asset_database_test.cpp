#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/renderer/asset_database.h"

namespace {

bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }

  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

bool write_temp_file(const char *path, const char *contents) {
  if ((path == nullptr) || (contents == nullptr)) {
    return false;
  }

  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }

  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

} // namespace

int main() {
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    return 100;
  }

  engine::renderer::clear_asset_database(database.get());

  constexpr engine::renderer::AssetId kAssetId = 77U;
  constexpr engine::renderer::MeshHandle kMeshHandle{5U};

  if (!engine::renderer::register_mesh_asset(database.get(), kAssetId,
                                             "assets/test.mesh", kMeshHandle)) {
    return 1;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
      engine::renderer::AssetState::Ready) {
    return 2;
  }

  if (!engine::renderer::mesh_asset_requested_resident(database.get(),
                                                       kAssetId)) {
    return 3;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId) !=
      kMeshHandle) {
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

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId) !=
      kMeshHandle) {
    return 9;
  }

  if (!engine::renderer::set_mesh_asset_state(
          database.get(), kAssetId, engine::renderer::AssetState::Loading,
          engine::renderer::kInvalidMeshHandle)) {
    return 10;
  }

  if (engine::renderer::mesh_asset_state(database.get(), kAssetId) !=
      engine::renderer::AssetState::Loading) {
    return 11;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId) !=
      engine::renderer::kInvalidMeshHandle) {
    return 12;
  }

  if (!engine::renderer::set_mesh_asset_state(
          database.get(), kAssetId, engine::renderer::AssetState::Failed,
          engine::renderer::kInvalidMeshHandle)) {
    return 13;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId) !=
      engine::renderer::kInvalidMeshHandle) {
    return 14;
  }

  if (engine::renderer::set_mesh_asset_state(
          database.get(), kAssetId, engine::renderer::AssetState::Ready,
          engine::renderer::kInvalidMeshHandle)) {
    return 15;
  }

  if (!engine::renderer::set_mesh_asset_state(
          database.get(), kAssetId, engine::renderer::AssetState::Ready,
          kMeshHandle)) {
    return 16;
  }

  if (engine::renderer::resolve_mesh_asset(database.get(), kAssetId) !=
      kMeshHandle) {
    return 17;
  }

  const char *tempA = "asset_db_hash_a.tmp";
  const char *tempB = "asset_db_hash_b.tmp";
  if (!write_temp_file(tempA, "mesh-v1") ||
      !write_temp_file(tempB, "mesh-v1")) {
    std::remove(tempA);
    std::remove(tempB);
    return 18;
  }

  const engine::renderer::AssetId idA1 =
      engine::renderer::make_asset_id_from_file(tempA);
  const engine::renderer::AssetId idB1 =
      engine::renderer::make_asset_id_from_file(tempB);
  if ((idA1 == engine::renderer::kInvalidAssetId) || (idA1 != idB1)) {
    std::remove(tempA);
    std::remove(tempB);
    return 19;
  }

  if (!write_temp_file(tempB, "mesh-v2")) {
    std::remove(tempA);
    std::remove(tempB);
    return 20;
  }

  const engine::renderer::AssetId idB2 =
      engine::renderer::make_asset_id_from_file(tempB);
  if ((idB2 == engine::renderer::kInvalidAssetId) || (idB2 == idB1)) {
    std::remove(tempA);
    std::remove(tempB);
    return 21;
  }

  const engine::renderer::AssetId missingFileId =
      engine::renderer::make_asset_id_from_file("definitely_missing.mesh");
  const engine::renderer::AssetId missingPathId =
      engine::renderer::make_asset_id_from_path("definitely_missing.mesh");
  std::remove(tempA);
  std::remove(tempB);
  if ((missingFileId == engine::renderer::kInvalidAssetId) ||
      (missingFileId != missingPathId)) {
    return 22;
  }

  return 0;
}
