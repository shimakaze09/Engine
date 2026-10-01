// Verifies that a search for assets of one type reaches every catalogued
// asset of that type: content::find_assets_of_type filters inside the
// catalog and reports the true number of matches, keeps the matches whose
// paths sort first whatever order they were catalogued in, and the editor
// bridge the reference pickers call finds the 600th texture of a project
// by its exact name and says how many matches it is not showing.

#include "engine/content/asset_catalog.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/service_registry.h"

#include "../test_harness.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

namespace ct = engine::content;

constexpr std::size_t kTextureCount = 600U;

/// "assets/textures/t042.png" for index 42.
void texture_path(std::size_t index, char *out, std::size_t outSize) noexcept {
  std::snprintf(out, outSize, "assets/textures/t%03u.png",
                static_cast<unsigned int>(index));
}

/// Catalogues kTextureCount textures out of path order (a stride walk, as
/// a filesystem walk may find them), with t599 catalogued last, past the
/// 512 records the pickers used to scan, plus a few meshes that share the
/// textures' names so a type filter has something to leave out.
bool fill(ct::AssetCatalog *catalog) noexcept {
  for (std::size_t step = 0U; step < kTextureCount; ++step) {
    const std::size_t index = ((step * 7U) + 6U) % kTextureCount;
    char path[64] = {};
    texture_path(index, path, sizeof(path));
    ct::AssetMetadata record{};
    record.assetId = ct::make_asset_id_from_path(path);
    record.typeTag = ct::AssetTypeTag::Texture;
    ct::write_metadata_path(&record.filePath, path);
    if (!ct::register_asset_metadata(catalog, record)) {
      return false;
    }
  }
  for (std::size_t i = 0U; i < 3U; ++i) {
    char path[64] = {};
    std::snprintf(path, sizeof(path), "assets/meshes/t%03u.mesh",
                  static_cast<unsigned int>(i));
    ct::AssetMetadata record{};
    record.assetId = ct::make_asset_id_from_path(path);
    record.typeTag = ct::AssetTypeTag::Mesh;
    ct::write_metadata_path(&record.filePath, path);
    if (!ct::register_asset_metadata(catalog, record)) {
      return false;
    }
  }
  return true;
}

/// True when `id` is the texture catalogued at `index`.
bool is_texture(ct::AssetId id, std::size_t index) noexcept {
  char path[64] = {};
  texture_path(index, path, sizeof(path));
  return id == ct::make_asset_id_from_path(path);
}

void check_catalog_search(engine::tests::TestContext &t,
                          const ct::AssetCatalog *catalog) noexcept {
  ct::AssetId ids[8] = {};
  t.check(ct::find_assets_of_type(catalog, ct::AssetTypeTag::Texture, "", ids,
                                  8U) == kTextureCount,
          "an empty query counts every texture");
  bool sorted = true;
  for (std::size_t i = 0U; i < 8U; ++i) {
    sorted = sorted && is_texture(ids[i], i);
  }
  t.check(sorted, "the kept matches are the first in path order");

  t.check(ct::find_assets_of_type(catalog, ct::AssetTypeTag::Texture,
                                  "T599.PNG", ids, 8U) == 1U &&
              is_texture(ids[0], 599U),
          "the last texture is found by its name in any case");
  t.check(ct::find_assets_of_type(catalog, ct::AssetTypeTag::Texture, "t59",
                                  ids, 8U) == 10U &&
              is_texture(ids[0], 590U) && is_texture(ids[7], 597U),
          "a partial name counts all ten and keeps the first eight");
  t.check(ct::find_assets_of_type(catalog, ct::AssetTypeTag::Mesh, "t00", ids,
                                  8U) == 3U,
          "the type filter leaves the textures out");
  t.check(ct::find_assets_of_type(catalog, ct::AssetTypeTag::Texture, nullptr,
                                  nullptr, 0U) == kTextureCount,
          "a count alone needs no output");
  t.check(ct::find_assets_of_type(catalog, ct::AssetTypeTag::Texture, "nothing",
                                  ids, 8U) == 0U,
          "a query nothing matches finds nothing");
  t.check(ct::find_assets_of_type(nullptr, ct::AssetTypeTag::Texture, "", ids,
                                  8U) == 0U,
          "no catalog finds nothing");
}

void check_picker_query(engine::tests::TestContext &t,
                        ct::AssetCatalog *catalog) noexcept {
  engine::runtime::EngineAssetDatabaseService service{};
  service.catalog = catalog;
  engine::runtime::set_editor_asset_service(&service);

  constexpr std::size_t kHits = 64U;
  engine::runtime::EditorAssetSearchResult hits[kHits];
  std::size_t total = 0U;
  std::size_t count = engine::runtime::editor_query_assets(
      ct::AssetTypeTag::Texture, "t599.png", hits, kHits, &total);
  t.check((count == 1U) && (total == 1U) &&
              (std::strcmp(hits[0].path, "assets/textures/t599.png") == 0),
          "the picker finds the 600th texture by its exact name");

  count = engine::runtime::editor_query_assets(ct::AssetTypeTag::Texture, "",
                                               hits, kHits, &total);
  t.check(
      (count == kHits) && (total == kTextureCount) &&
          (std::strcmp(hits[0].path, "assets/textures/t000.png") == 0) &&
          (std::strcmp(hits[kHits - 1U].path, "assets/textures/t063.png") == 0),
      "an empty query shows the first 64 by path and counts all 600");

  engine::runtime::set_editor_asset_service(nullptr);
  count = engine::runtime::editor_query_assets(ct::AssetTypeTag::Texture, "",
                                               hits, kHits, &total);
  t.check((count == 0U) && (total == 0U),
          "with no asset service the query is empty");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  std::unique_ptr<ct::AssetCatalog> catalog(new (std::nothrow)
                                                ct::AssetCatalog());
  if ((catalog == nullptr) || !fill(catalog.get())) {
    std::fprintf(stderr, "FAIL: the catalog could not be filled\n");
    return 1;
  }
  check_catalog_search(t, catalog.get());
  check_picker_query(t, catalog.get());
  return t.finish("asset_type_query");
}
