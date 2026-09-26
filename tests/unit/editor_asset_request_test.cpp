// Verifies the editor-facing asset entry points: no published service
// yields no id, a published service turns a virtual path into the
// path-derived asset id with the database marked Loading, and an asset's
// persistent identity is reported as the catalog holds it -- never
// invented for an asset that carries none, because an editor gesture
// writes that identity into a saved document.

#include "engine/content/asset_catalog.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/service_registry.h"

#include <cstdio>
#include <memory>
#include <new>

namespace {

constexpr const char *kMountPrefix = "edtest";
constexpr const char *kVirtualPath = "edtest/prop.mesh";

/// Without a published service every request must fail with id 0.
int check_request_without_service_fails() noexcept {
  engine::runtime::set_editor_asset_service(nullptr);
  if (engine::runtime::editor_request_mesh_asset(kVirtualPath) != 0ULL) {
    return 1;
  }
  if (engine::runtime::editor_request_mesh_asset(nullptr) != 0ULL) {
    return 2;
  }
  return 0;
}

/// With a service published the request must return the path-derived id
/// and mark the asset Loading in the database.
int check_request_marks_asset_loading() noexcept {
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    return 10;
  }

  if (!engine::core::initialize_vfs()) {
    return 11;
  }

  engine::runtime::EngineAssetDatabaseService service{};
  std::unique_ptr<engine::content::AssetCatalog> serviceCatalog(
      new (std::nothrow) engine::content::AssetCatalog());
  service.catalog = serviceCatalog.get();
  service.database = database.get();
  engine::runtime::set_editor_asset_service(&service);
  const auto finish = [](int result) noexcept {
    engine::runtime::set_editor_asset_service(nullptr);
    engine::core::shutdown_vfs();
    return result;
  };

  if (!engine::core::mount(kMountPrefix, ".")) {
    return finish(12);
  }

  const std::uint64_t assetId =
      engine::runtime::editor_request_mesh_asset(kVirtualPath);
  const std::uint64_t expectedId =
      engine::content::make_asset_id_from_path(kVirtualPath);
  if ((assetId == 0ULL) || (assetId != expectedId)) {
    return finish(13);
  }
  if (engine::renderer::mesh_asset_state(database.get(), assetId) !=
      engine::content::AssetState::Loading) {
    return finish(14);
  }

  const std::uint64_t repeated =
      engine::runtime::editor_request_mesh_asset(kVirtualPath);
  if (repeated != assetId) {
    return finish(15);
  }

  if (engine::runtime::editor_request_mesh_asset("unmounted/prop.mesh") !=
      0ULL) {
    return finish(16);
  }

  return finish(0);
}

/// A gesture that points a component at an asset needs the identity a
/// saved document will name. The bridge reports what the catalog holds
/// and nothing more: no service, an unknown id, and a record registered
/// without an identity all read as a nil reference, while a registered
/// identity comes back whole.
int check_asset_ref_reports_only_catalogued_identity() noexcept {
  engine::runtime::set_editor_asset_service(nullptr);
  if (engine::core::asset_ref_is_valid(engine::runtime::editor_asset_ref(7ULL))) {
    return 20;
  }

  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    return 21;
  }
  engine::runtime::EngineAssetDatabaseService service{};
  std::unique_ptr<engine::content::AssetCatalog> serviceCatalog(
      new (std::nothrow) engine::content::AssetCatalog());
  service.catalog = serviceCatalog.get();
  service.database = database.get();
  engine::runtime::set_editor_asset_service(&service);
  const auto finish = [](int result) noexcept {
    engine::runtime::set_editor_asset_service(nullptr);
    return result;
  };

  // A nil id and an id no record answers for are both nil, not a guess.
  if (engine::core::asset_ref_is_valid(engine::runtime::editor_asset_ref(0ULL)) ||
      engine::core::asset_ref_is_valid(
          engine::runtime::editor_asset_ref(0xABCDEFULL))) {
    return finish(22);
  }

  // A record the mount walk could give no identity stays identity-less
  // here: a made-up reference would name a different asset next run.
  engine::content::AssetMetadata unidentified{};
  unidentified.assetId = 101ULL;
  unidentified.typeTag = engine::content::AssetTypeTag::Mesh;
  engine::content::write_metadata_path(&unidentified.filePath,
                                       "edtest/unimported.mesh");
  if (!engine::content::register_asset_metadata(service.catalog,
                                                unidentified)) {
    return finish(23);
  }
  if (engine::core::asset_ref_is_valid(
          engine::runtime::editor_asset_ref(101ULL))) {
    return finish(24);
  }

  // A catalogued identity comes back exactly, sub-asset local id included.
  constexpr engine::core::AssetRef kRef{
      engine::core::AssetGuid{0x0123456789abcdefULL, 0xfedcba9876543210ULL},
      0x432408a2e33116bcULL};
  engine::content::AssetMetadata identified{};
  identified.assetId = 202ULL;
  identified.typeTag = engine::content::AssetTypeTag::Mesh;
  identified.ref = kRef;
  engine::content::write_metadata_path(&identified.filePath,
                                       "edtest/imported.mesh");
  if (!engine::content::register_asset_metadata(service.catalog, identified)) {
    return finish(25);
  }
  if (!(engine::runtime::editor_asset_ref(202ULL) == kRef)) {
    return finish(26);
  }

  return finish(0);
}

} // namespace

int main() {
  int result = check_request_without_service_fails();
  if (result != 0) {
    std::fprintf(stderr, "editor_asset_request_test failed: %d\n", result);
    return result;
  }

  result = check_request_marks_asset_loading();
  if (result != 0) {
    std::fprintf(stderr, "editor_asset_request_test failed: %d\n", result);
    return result;
  }

  result = check_asset_ref_reports_only_catalogued_identity();
  if (result != 0) {
    std::fprintf(stderr, "editor_asset_request_test failed: %d\n", result);
    return result;
  }

  std::printf("editor_asset_request_test: all tests passed\n");
  return 0;
}
