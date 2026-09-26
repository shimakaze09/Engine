// Proves the #171 closure criterion that generic asset metadata works with
// no renderer dependency: this target links engine_content only. Covers the
// moved AssetCatalog contract — registration/lookup round trip, tags, a
// cross-type dependency edge (Script -> Mesh, the renderer/non-render
// crossing #171 requires), dependency-ordered load with cycle rejection,
// and the shared path-hash identity constructor. Also covers what the
// catalog adds as the engine's one asset record (#680): the change
// generation, the keep-first write rule, the capacity probe and the
// lookup by path.

#include "engine/content/asset_catalog.h"
#include "engine/content/asset_metadata.h"

#include <cstdint>
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
  std::unique_ptr<AssetCatalog> store(new (std::nothrow) AssetCatalog());
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
  clear_asset_catalog(store.get());
  CHECK(find_asset_metadata(store.get(), scriptId) == nullptr,
        "clear empties the table");

  // #570: the fixed-width tag and path fields are compared with strcmp, so
  // a caller-filled array without a terminator must be refused at
  // registration, leaving the store unchanged.
  AssetMetadata unterminatedTag =
      make_meta(meshId, AssetTypeTag::Mesh, "assets/props/rock.mesh");
  unterminatedTag.tagCount = 1U;
  unterminatedTag.tags[0].fill('a');
  CHECK(!register_asset_metadata(store.get(), unterminatedTag),
        "a tag without a terminator is refused");
  AssetMetadata unterminatedPath =
      make_meta(meshId, AssetTypeTag::Mesh, "assets/props/rock.mesh");
  unterminatedPath.filePath.fill('p');
  CHECK(!register_asset_metadata(store.get(), unterminatedPath),
        "a path without a terminator is refused");
  CHECK(find_asset_metadata(store.get(), meshId) == nullptr,
        "a refused registration leaves the store unchanged");

  // --- The catalog's change generation moves once per landed write. ---
  clear_asset_catalog(store.get());
  std::uint64_t generation = store->generation;
  CHECK(
      register_asset_metadata(store.get(), make_meta(meshId, AssetTypeTag::Mesh,
                                                     "assets/props/rock.mesh")),
      "register after clear");
  CHECK(store->generation == generation + 1U,
        "a register moves the generation once");
  generation = store->generation;
  CHECK(!register_asset_metadata(store.get(), unterminatedPath),
        "a malformed record is still refused");
  CHECK(store->generation == generation,
        "a refused register leaves the generation");
  CHECK(add_asset_tag(store.get(), meshId, "prop"), "tag the mesh");
  CHECK(store->generation == generation + 1U, "a new tag moves it once");
  generation = store->generation;
  CHECK(add_asset_tag(store.get(), meshId, "prop"), "the same tag again");
  CHECK(store->generation == generation,
        "a tag the record already carries is no write");
  CHECK(add_asset_dependency(store.get(), meshId, scriptId), "a new edge");
  CHECK(store->generation == generation + 1U, "a new edge moves it once");
  generation = store->generation;
  CHECK(add_asset_dependency(store.get(), meshId, scriptId), "the same edge");
  CHECK(store->generation == generation,
        "an edge the record already carries is no write");
  CHECK(!add_asset_tag(store.get(), scriptId, "absent"),
        "tagging an uncatalogued id is refused");
  CHECK(store->generation == generation, "a refused tag leaves the generation");

  // --- Keep-first versus replace. ---
  AssetMetadata richer =
      make_meta(meshId, AssetTypeTag::Mesh, "assets/props/rock.mesh");
  richer.fileSize = 42U;
  CHECK(register_asset_metadata_if_absent(store.get(), richer) ==
            CatalogInsert::AlreadyKnown,
        "if-absent reports a known id");
  CHECK(find_asset_metadata(store.get(), meshId)->fileSize == 0U,
        "if-absent keeps the first record");
  CHECK(store->generation == generation, "a kept record is no write");
  CHECK(register_asset_metadata(store.get(), richer), "replace the record");
  CHECK(find_asset_metadata(store.get(), meshId)->fileSize == 42U,
        "register replaces the whole record");
  CHECK(register_asset_metadata_if_absent(
            store.get(), make_meta(scriptId, AssetTypeTag::Script,
                                   "assets/scripts/ai.lua")) ==
            CatalogInsert::Registered,
        "if-absent adds an unknown id");

  // --- Lookup by path, by canonical spelling. ---
  CHECK(find_asset_metadata_by_path(store.get(), "assets/props/rock.mesh") ==
            find_asset_metadata(store.get(), meshId),
        "the path finds its record");
  CHECK(find_asset_metadata_by_path(store.get(), "assets//props/./rock.mesh") ==
            find_asset_metadata(store.get(), meshId),
        "another spelling of the path finds it too");
  CHECK(find_asset_metadata_by_path(store.get(), "assets/props/Rock.mesh") ==
            nullptr,
        "case is part of the path");
  CHECK(find_asset_metadata_by_path(store.get(), "assets/props/none.mesh") ==
            nullptr,
        "an uncatalogued path finds nothing");
  CHECK(find_asset_metadata_by_path(store.get(), "../rock.mesh") == nullptr,
        "a path that names no asset finds nothing");
  // A record whose stored path is not the path its id hashes (a colliding
  // id) must not answer for the path asked.
  AssetMetadata impostor = make_meta(make_asset_id_from_path("assets/a.png"),
                                     AssetTypeTag::Texture, "assets/b.png");
  CHECK(register_asset_metadata(store.get(), impostor), "plant a mismatch");
  CHECK(find_asset_metadata_by_path(store.get(), "assets/a.png") == nullptr,
        "a record at another path never answers for this one");

  // --- The capacity probe agrees with register at the boundary. ---
  clear_asset_catalog(store.get());
  for (std::size_t i = 0U; i < AssetCatalog::kMaxMetadata; ++i) {
    const AssetId id = static_cast<AssetId>(i + 1U);
    char path[32] = {};
    std::snprintf(path, sizeof(path), "assets/fill/%zu.png", i);
    if (!register_asset_metadata(store.get(),
                                 make_meta(id, AssetTypeTag::Texture, path))) {
      CHECK(false, "fill the catalog to capacity");
      break;
    }
  }
  const AssetId extra = static_cast<AssetId>(AssetCatalog::kMaxMetadata + 1U);
  CHECK(!can_register_asset_metadata(store.get(), extra),
        "a full catalog has no slot for a new id");
  CHECK(
      !register_asset_metadata(
          store.get(), make_meta(extra, AssetTypeTag::Texture, "assets/x.png")),
      "and register agrees");
  CHECK(can_register_asset_metadata(store.get(), 1U),
        "a known id can still be replaced when full");
  CHECK(register_asset_metadata(
            store.get(), make_meta(1U, AssetTypeTag::Texture, "assets/y.png")),
        "and register agrees");
  CHECK(!can_register_asset_metadata(store.get(), kInvalidAssetId),
        "the invalid id never registers");

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("content_metadata_test passed");
  return 0;
}
