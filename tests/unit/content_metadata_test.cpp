// Proves the #171 closure criterion that generic asset metadata works with
// no renderer dependency: this target links engine_content only. Covers the
// moved MetadataStore contract — registration/lookup round trip, tags, a
// cross-type dependency edge (Script -> Mesh, the renderer/non-render
// crossing #171 requires), dependency-ordered load with cycle rejection,
// and the shared path-hash identity constructor.

#include "engine/content/asset_metadata.h"
#include "engine/content/metadata_store.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

engine::content::AssetMetadata make_meta(engine::content::AssetId id,
                                         engine::content::AssetTypeTag tag,
                                         const char *path) {
  engine::content::AssetMetadata meta{};
  meta.assetId = id;
  meta.typeTag = tag;
  std::snprintf(meta.filePath.data(), meta.filePath.size(), "%s", path);
  return meta;
}

int g_loadOrderCount = 0;
engine::content::AssetId g_loadOrder[8] = {};

bool record_load(engine::content::AssetId id, void *userData) noexcept {
  static_cast<void>(userData);
  if (g_loadOrderCount < 8) {
    g_loadOrder[g_loadOrderCount] = id;
  }
  ++g_loadOrderCount;
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  using namespace engine::content;

  // ~16 MB table: heap-allocate like production owners do.
  std::unique_ptr<MetadataStore> store(new (std::nothrow) MetadataStore());
  CHECK(store != nullptr, "store allocation");

  // Identity: deterministic, separator-canonicalized, never invalid.
  const AssetId scriptId = make_asset_id_from_path("assets/scripts/ai.lua");
  const AssetId meshId = make_asset_id_from_path("assets/props/rock.mesh");
  CHECK(scriptId != kInvalidAssetId && meshId != kInvalidAssetId,
        "path ids are valid");
  CHECK(make_asset_id_from_path("assets\\scripts\\ai.lua") == scriptId,
        "backslash paths hash identically");

  // Registration round trip across asset classes.
  CHECK(register_asset_metadata(
            store.get(),
            make_meta(scriptId, AssetTypeTag::Script, "assets/scripts/ai.lua")),
        "register script metadata");
  CHECK(register_asset_metadata(
            store.get(),
            make_meta(meshId, AssetTypeTag::Mesh, "assets/props/rock.mesh")),
        "register mesh metadata");
  const AssetMetadata *found = find_asset_metadata(store.get(), scriptId);
  CHECK((found != nullptr) && (found->typeTag == AssetTypeTag::Script),
        "script metadata round trips");

  // Tags.
  CHECK(add_asset_tag(store.get(), scriptId, "gameplay"), "tag add");
  CHECK(asset_has_tag(store.get(), scriptId, "gameplay"), "tag query");
  AssetId tagged[4] = {};
  CHECK(query_assets_by_tag(store.get(), "gameplay", tagged, 4U) == 1U &&
            tagged[0] == scriptId,
        "tag scan finds exactly the script");

  // Type query.
  AssetId meshes[4] = {};
  CHECK(query_assets_by_type(store.get(), AssetTypeTag::Mesh, meshes, 4U) ==
            1U && meshes[0] == meshId,
        "type scan finds exactly the mesh");

  // Cross-type dependency: the script depends on the mesh.
  CHECK(add_asset_dependency(store.get(), scriptId, meshId),
        "cross-type dependency edge records");
  AssetId deps[4] = {};
  CHECK(get_dependencies(store.get(), scriptId, deps, 4U) == 1U &&
            deps[0] == meshId,
        "dependency query returns the mesh");

  // Dependency-ordered load: mesh before script.
  g_loadOrderCount = 0;
  CHECK(load_with_dependencies(store.get(), scriptId, &record_load, nullptr),
        "dependency-ordered load succeeds");
  CHECK(g_loadOrderCount == 2 && g_loadOrder[0] == meshId &&
            g_loadOrder[1] == scriptId,
        "dependencies load before the dependent");

  // Cycle rejection.
  CHECK(add_asset_dependency(store.get(), meshId, scriptId),
        "reverse edge records");
  g_loadOrderCount = 0;
  CHECK(!load_with_dependencies(store.get(), scriptId, &record_load, nullptr),
        "cycle is rejected");

  // Reset boundary.
  clear_metadata_store(store.get());
  CHECK(find_asset_metadata(store.get(), scriptId) == nullptr,
        "clear empties the table");

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("content_metadata_test passed");
  return 0;
}
