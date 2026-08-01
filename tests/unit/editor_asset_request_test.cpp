// Verifies the editor-facing mesh asset request entry point: no published
// service yields no id, and a published service turns a virtual path into
// the path-derived asset id with the database marked Loading.

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
      engine::renderer::make_asset_id_from_path(kVirtualPath);
  if ((assetId == 0ULL) || (assetId != expectedId)) {
    return finish(13);
  }
  if (engine::renderer::mesh_asset_state(database.get(), assetId) !=
      engine::renderer::AssetState::Loading) {
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

  std::printf("editor_asset_request_test: all tests passed\n");
  return 0;
}
