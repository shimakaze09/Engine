// Verifies asset database test behavior for the Engine test suite.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/renderer/asset_database.h"

namespace {

/// Handles open file for write.
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

/// Writes temp file data.
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

/// Verifies unregister_mesh_asset frees slots for reuse without breaking
/// probe chains (regression coverage for unbounded slot growth).
int verify_mesh_slot_reclamation() {
  using engine::renderer::AssetDatabase;
  using engine::renderer::AssetId;
  using engine::renderer::AssetState;
  using engine::renderer::MeshHandle;

  std::unique_ptr<AssetDatabase> database(new (std::nothrow) AssetDatabase());
  if (database == nullptr) {
    return 200;
  }
  engine::renderer::clear_asset_database(database.get());

  // Ids spaced exactly kMaxMeshAssets apart share a home slot, forcing one
  // probe chain through every record.
  constexpr AssetId kBase = 11ULL;
  const AssetId first = kBase;
  const AssetId second = kBase + AssetDatabase::kMaxMeshAssets;
  const AssetId third = kBase + 2ULL * AssetDatabase::kMaxMeshAssets;

  for (const AssetId id : {first, second, third}) {
    if (!engine::renderer::register_mesh_asset(database.get(), id,
                                               "assets/chain.mesh",
                                               MeshHandle{7U})) {
      return 201;
    }
  }

  // A referenced record (register leaves refCount=1, live mesh) must refuse.
  if (engine::renderer::unregister_mesh_asset(database.get(), second)) {
    return 202;
  }

  // Drop the reference and GPU handle, then unregister the chain's middle.
  if (!engine::renderer::release_mesh_asset(database.get(), second)) {
    return 203;
  }
  if (!engine::renderer::set_mesh_asset_state(
          database.get(), second, AssetState::Unloaded,
          engine::renderer::kInvalidMeshHandle)) {
    return 204;
  }
  if (!engine::renderer::unregister_mesh_asset(database.get(), second)) {
    return 205;
  }

  // The record after the tombstone must still resolve through the chain.
  if (engine::renderer::mesh_asset_state(database.get(), third) !=
      AssetState::Ready) {
    return 206;
  }
  if (engine::renderer::mesh_asset_state(database.get(), second) !=
      AssetState::Unloaded) {
    return 207; // unregistered id must read as absent/unloaded
  }

  // The freed slot must be reusable by a new asset.
  const AssetId fresh = kBase + 3ULL * AssetDatabase::kMaxMeshAssets;
  if (!engine::renderer::register_mesh_asset(database.get(), fresh,
                                             "assets/fresh.mesh",
                                             MeshHandle{9U})) {
    return 208;
  }
  if (engine::renderer::mesh_asset_state(database.get(), fresh) !=
      AssetState::Ready) {
    return 209;
  }

  // Double unregister reports failure.
  if (engine::renderer::unregister_mesh_asset(database.get(), second)) {
    return 210;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  const int reclamation = verify_mesh_slot_reclamation();
  if (reclamation != 0) {
    std::fprintf(stderr, "mesh slot reclamation failed: %d\n", reclamation);
    return reclamation;
  }
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    return 100;
  }

  engine::renderer::clear_asset_database(database.get());

  constexpr engine::renderer::AssetId kAssetId = 77ULL;
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

  constexpr engine::renderer::AssetId kStreamingAssetId = 78ULL;
  if (!engine::renderer::request_mesh_asset_streaming_load(
          database.get(), kStreamingAssetId, "assets/streamed.mesh")) {
    return 28;
  }
  if (!engine::renderer::mesh_asset_requested_resident(database.get(),
                                                       kStreamingAssetId)) {
    return 29;
  }
  if (engine::renderer::mesh_asset_state(database.get(), kStreamingAssetId) !=
      engine::renderer::AssetState::Loading) {
    return 30;
  }
  if (engine::renderer::resolve_mesh_asset(database.get(),
                                           kStreamingAssetId) !=
      engine::renderer::kInvalidMeshHandle) {
    return 31;
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

  engine::renderer::AssetMetadata validMetadata{};
  validMetadata.assetId = 88ULL;
  validMetadata.tagCount = engine::renderer::AssetMetadata::kMaxTags;
  validMetadata.dependencyCount =
      engine::renderer::AssetMetadata::kMaxDependencies;
  if (!engine::renderer::register_asset_metadata(database.get(),
                                                 validMetadata)) {
    return 23;
  }

  engine::renderer::AssetMetadata invalidTags{};
  invalidTags.assetId = 89ULL;
  invalidTags.tagCount = engine::renderer::AssetMetadata::kMaxTags + 1U;
  if (engine::renderer::register_asset_metadata(database.get(), invalidTags)) {
    return 24;
  }
  if (engine::renderer::find_asset_metadata(database.get(),
                                            invalidTags.assetId) != nullptr) {
    return 25;
  }

  engine::renderer::AssetMetadata invalidDeps{};
  invalidDeps.assetId = 90ULL;
  invalidDeps.dependencyCount =
      engine::renderer::AssetMetadata::kMaxDependencies + 1U;
  if (engine::renderer::register_asset_metadata(database.get(), invalidDeps)) {
    return 26;
  }
  if (engine::renderer::find_asset_metadata(database.get(),
                                            invalidDeps.assetId) != nullptr) {
    return 27;
  }

  return 0;
}
