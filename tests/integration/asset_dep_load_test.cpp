#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "engine/renderer/asset_database.h"

namespace {

/// Track the order in which assets are loaded.
struct LoadTracker {
  std::vector<engine::renderer::AssetId> loadOrder{};
};

bool tracking_load_callback(engine::renderer::AssetDatabase *db,
                            engine::renderer::AssetId id, void *userData) {
  (void)db;
  if (userData == nullptr) {
    return false;
  }
  auto *tracker = static_cast<LoadTracker *>(userData);
  tracker->loadOrder.push_back(id);
  return true;
}

bool failing_load_callback(engine::renderer::AssetDatabase * /*db*/,
                           engine::renderer::AssetId id, void *userData) {
  // Fail on a specific asset.
  const auto failId = *static_cast<engine::renderer::AssetId *>(userData);
  return id != failId;
}

} // namespace

int main() {
  using namespace engine::renderer;

  std::unique_ptr<AssetDatabase> database(new (std::nothrow) AssetDatabase());
  if (database == nullptr) {
    return 100;
  }
  clear_asset_database(database.get());

  // --- Test 1: Prefab -> Mesh -> Texture chain ---
  // Setup: prefab(100) depends on mesh(200), mesh(200) depends on texture(300).
  {
    AssetMetadata prefabMeta{};
    prefabMeta.assetId = 100ULL;
    prefabMeta.typeTag = AssetTypeTag::Prefab;
    write_metadata_path(&prefabMeta.filePath, "assets/hero.prefab");
    if (!register_asset_metadata(database.get(), prefabMeta)) {
      return 1;
    }
    if (!add_asset_dependency(database.get(), 100ULL, 200ULL)) {
      return 2;
    }

    AssetMetadata meshMeta{};
    meshMeta.assetId = 200ULL;
    meshMeta.typeTag = AssetTypeTag::Mesh;
    write_metadata_path(&meshMeta.filePath, "assets/hero.mesh");
    if (!register_asset_metadata(database.get(), meshMeta)) {
      return 3;
    }
    if (!add_asset_dependency(database.get(), 200ULL, 300ULL)) {
      return 4;
    }

    AssetMetadata texMeta{};
    texMeta.assetId = 300ULL;
    texMeta.typeTag = AssetTypeTag::Texture;
    write_metadata_path(&texMeta.filePath, "assets/hero_diffuse.png");
    if (!register_asset_metadata(database.get(), texMeta)) {
      return 5;
    }

    // Load prefab with dependency tracking.
    LoadTracker tracker{};
    if (!load_with_dependencies(database.get(), 100ULL, tracking_load_callback,
                                &tracker)) {
      return 6;
    }

    // Verify load order: texture(300) first, then mesh(200), then prefab(100).
    if (tracker.loadOrder.size() != 3U) {
      std::fprintf(stderr, "expected 3 loads, got %zu\n",
                   tracker.loadOrder.size());
      return 7;
    }
    if (tracker.loadOrder[0] != 300ULL) {
      std::fprintf(stderr, "expected texture(300) first, got %llu\n",
                   static_cast<unsigned long long>(tracker.loadOrder[0]));
      return 8;
    }
    if (tracker.loadOrder[1] != 200ULL) {
      std::fprintf(stderr, "expected mesh(200) second, got %llu\n",
                   static_cast<unsigned long long>(tracker.loadOrder[1]));
      return 9;
    }
    if (tracker.loadOrder[2] != 100ULL) {
      std::fprintf(stderr, "expected prefab(100) third, got %llu\n",
                   static_cast<unsigned long long>(tracker.loadOrder[2]));
      return 10;
    }
  }

  // --- Test 2: Diamond dependency (shared texture) ---
  // A(1) -> B(2) -> D(4)
  // A(1) -> C(3) -> D(4)
  // D should only be loaded once.
  {
    clear_asset_database(database.get());

    AssetMetadata metaA{};
    metaA.assetId = 1ULL;
    metaA.typeTag = AssetTypeTag::Prefab;
    write_metadata_path(&metaA.filePath, "A");
    register_asset_metadata(database.get(), metaA);
    add_asset_dependency(database.get(), 1ULL, 2ULL);
    add_asset_dependency(database.get(), 1ULL, 3ULL);

    AssetMetadata metaB{};
    metaB.assetId = 2ULL;
    metaB.typeTag = AssetTypeTag::Mesh;
    write_metadata_path(&metaB.filePath, "B");
    register_asset_metadata(database.get(), metaB);
    add_asset_dependency(database.get(), 2ULL, 4ULL);

    AssetMetadata metaC{};
    metaC.assetId = 3ULL;
    metaC.typeTag = AssetTypeTag::Mesh;
    write_metadata_path(&metaC.filePath, "C");
    register_asset_metadata(database.get(), metaC);
    add_asset_dependency(database.get(), 3ULL, 4ULL);

    AssetMetadata metaD{};
    metaD.assetId = 4ULL;
    metaD.typeTag = AssetTypeTag::Texture;
    write_metadata_path(&metaD.filePath, "D");
    register_asset_metadata(database.get(), metaD);

    LoadTracker tracker{};
    if (!load_with_dependencies(database.get(), 1ULL, tracking_load_callback,
                                &tracker)) {
      return 20;
    }

    // D must appear before B and C; B and C before A.
    // Exact order of B/C may vary, but D must be first, A must be last.
    if (tracker.loadOrder.size() < 4U) {
      std::fprintf(stderr, "expected >=4 loads, got %zu\n",
                   tracker.loadOrder.size());
      return 21;
    }

    // A must be last.
    if (tracker.loadOrder.back() != 1ULL) {
      return 22;
    }

    // D must appear before B and C.
    std::size_t posD = 999U;
    std::size_t posB = 999U;
    std::size_t posC = 999U;
    for (std::size_t i = 0U; i < tracker.loadOrder.size(); ++i) {
      if (tracker.loadOrder[i] == 4ULL) {
        posD = i;
      }
      if (tracker.loadOrder[i] == 2ULL) {
        posB = i;
      }
      if (tracker.loadOrder[i] == 3ULL) {
        posC = i;
      }
    }
    if ((posD >= posB) || (posD >= posC)) {
      return 23;
    }
  }

  // --- Test 3: Circular dependency detection ---
  {
    clear_asset_database(database.get());

    AssetMetadata metaX{};
    metaX.assetId = 10ULL;
    metaX.typeTag = AssetTypeTag::Mesh;
    write_metadata_path(&metaX.filePath, "X");
    register_asset_metadata(database.get(), metaX);

    AssetMetadata metaY{};
    metaY.assetId = 20ULL;
    metaY.typeTag = AssetTypeTag::Texture;
    write_metadata_path(&metaY.filePath, "Y");
    register_asset_metadata(database.get(), metaY);

    // Create a cycle: X -> Y -> X.
    add_asset_dependency(database.get(), 10ULL, 20ULL);
    add_asset_dependency(database.get(), 20ULL, 10ULL);

    LoadTracker tracker{};
    // Should fail due to cycle.
    if (load_with_dependencies(database.get(), 10ULL, tracking_load_callback,
                               &tracker)) {
      return 30; // Expected failure.
    }
  }

  // --- Test 4: Load failure propagation ---
  {
    clear_asset_database(database.get());

    AssetMetadata metaRoot{};
    metaRoot.assetId = 50ULL;
    metaRoot.typeTag = AssetTypeTag::Prefab;
    write_metadata_path(&metaRoot.filePath, "root");
    register_asset_metadata(database.get(), metaRoot);
    add_asset_dependency(database.get(), 50ULL, 60ULL);

    AssetMetadata metaDep{};
    metaDep.assetId = 60ULL;
    metaDep.typeTag = AssetTypeTag::Mesh;
    write_metadata_path(&metaDep.filePath, "dep");
    register_asset_metadata(database.get(), metaDep);

    // Load callback that fails on asset 60.
    AssetId failId = 60ULL;
    if (load_with_dependencies(database.get(), 50ULL, failing_load_callback,
                               &failId)) {
      return 40; // Expected failure.
    }
  }

  // --- Test 5: Null/invalid inputs ---
  {
    if (load_with_dependencies(nullptr, 1ULL, tracking_load_callback,
                               nullptr)) {
      return 50;
    }
    if (load_with_dependencies(database.get(), 0ULL, tracking_load_callback,
                               nullptr)) {
      return 51;
    }
  }

  // --- Test 6: get_dependencies query ---
  {
    clear_asset_database(database.get());

    AssetMetadata meta{};
    meta.assetId = 77ULL;
    meta.typeTag = AssetTypeTag::Prefab;
    write_metadata_path(&meta.filePath, "test");
    register_asset_metadata(database.get(), meta);
    add_asset_dependency(database.get(), 77ULL, 88ULL);
    add_asset_dependency(database.get(), 77ULL, 99ULL);

    AssetId deps[8] = {};
    const std::size_t count = get_dependencies(database.get(), 77ULL, deps, 8);
    if (count != 2U) {
      return 60;
    }

    bool found88 = false;
    bool found99 = false;
    for (std::size_t i = 0U; i < count; ++i) {
      if (deps[i] == 88ULL) {
        found88 = true;
      }
      if (deps[i] == 99ULL) {
        found99 = true;
      }
    }
    if (!found88 || !found99) {
      return 61;
    }
  }

  // --- Test 7: Asset with no metadata still works ---
  {
    clear_asset_database(database.get());

    // Loading an asset with no metadata should still succeed (no deps).
    LoadTracker tracker{};
    if (!load_with_dependencies(database.get(), 999ULL, tracking_load_callback,
                                &tracker)) {
      return 70;
    }
    // Should have loaded just the root.
    if (tracker.loadOrder.size() != 1U) {
      return 71;
    }
    if (tracker.loadOrder[0] != 999ULL) {
      return 72;
    }
  }

  std::printf("asset_dep_load_test: all tests passed\n");
  return 0;
}
