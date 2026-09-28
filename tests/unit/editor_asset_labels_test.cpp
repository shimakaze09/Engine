// Verifies asset labels in the editor on a scratch project: the index walk
// reads each entry's labels from its own sidecar (a cooked output has
// none), the Assets search keeps entries carrying every "l:<label>" term,
// matched without case and across folders, with the rest of the query
// still a name search, and a label edit writes the sidecar (keeping its
// import settings), updates the index so the search sees it at once, and
// moves the catalog's tags from the old labels to the new while leaving
// other tags alone. An asset with no sidecar is refused with nothing
// written.

#include "editor_asset_index.h"
#include "editor_asset_labels.h"

#include "engine/content/asset_catalog.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/service_registry.h"

#include "../test_harness.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace engine::editor;
namespace ct = engine::content;

bool write_file(const std::filesystem::path &path, const char *text) noexcept {
  std::error_code ec{};
  std::filesystem::create_directories(path.parent_path(), ec);
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.string().c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(text);
  const bool written = std::fwrite(text, 1U, length, file) == length;
  return (std::fclose(file) == 0) && written;
}

bool labelled_sidecar(const std::filesystem::path &asset,
                      std::initializer_list<const char *> labels,
                      bool meshImport = false) noexcept {
  ct::AssetSidecar sidecar{};
  sidecar.guid = ct::generate_asset_guid();
  sidecar.hasMeshImport = meshImport;
  sidecar.meshImport.scaleFactor = 3.0F;
  for (const char *label : labels) {
    if (!ct::asset_labels_add(&sidecar.labels, label)) {
      return false;
    }
  }
  return ct::write_asset_sidecar(asset.string().c_str(), sidecar);
}

const AssetIndexEntry *entry_named(const char *name) noexcept {
  for (std::size_t i = 0U; i < asset_index_count(); ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    if ((entry != nullptr) && (std::strcmp(entry->name, name) == 0)) {
      return entry;
    }
  }
  return nullptr;
}

/// The names the search `query` keeps, sorted and joined by spaces.
std::string search(const char *query) noexcept {
  AssetFilterState filter{};
  std::snprintf(filter.query, sizeof(filter.query), "%s", query);
  std::vector<std::string> names{};
  for (std::size_t i = 0U; i < asset_index_count(); ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    if ((entry != nullptr) && asset_entry_matches_filter(*entry, filter)) {
      names.emplace_back(entry->name);
    }
  }
  std::sort(names.begin(), names.end());
  std::string joined{};
  for (const std::string &name : names) {
    joined += (joined.empty() ? "" : " ") + name;
  }
  return joined;
}

bool build_tree() noexcept {
  const std::filesystem::path root("assets");
  std::error_code ec{};
  std::filesystem::remove_all(root, ec);
  return write_file(root / "rock.png", "x") &&
         write_file(root / "nature/tree.png", "x") &&
         write_file(root / "hero.gltf", "{}") &&
         write_file(root / "hero.mesh", "mesh") &&
         write_file(root / "rocket.lua", "-- script") &&
         labelled_sidecar(root / "rock.png", {"env", "Rock"}) &&
         labelled_sidecar(root / "nature/tree.png", {"ENV"}) &&
         labelled_sidecar(root / "hero.gltf", {}, true) &&
         labelled_sidecar(root / "rocket.lua", {});
}

void check_walk_and_search(engine::tests::TestContext &t) noexcept {
  t.check(build_tree() && rebuild_asset_index(), "the scratch project indexes");
  const AssetIndexEntry *rock = entry_named("rock.png");
  const AssetIndexEntry *mesh = entry_named("hero.mesh");
  const AssetIndexEntry *source = entry_named("hero.gltf");
  t.check((rock != nullptr) && rock->hasSidecar && (rock->labels.count == 2U) &&
              (std::strcmp(rock->labels.names[1].data(), "Rock") == 0),
          "an entry carries the labels its sidecar holds");
  t.check((mesh != nullptr) && !mesh->hasSidecar &&
              (mesh->labels.count == 0U) && (source != nullptr) &&
              source->hasSidecar && (source->labels.count == 0U),
          "a cooked output has no sidecar and no labels; its source has one");

  t.check(search("l:env") == "rock.png tree.png",
          "l:env keeps every entry labelled env, in any case and any folder");
  t.check(search("l:ENV l:rock") == "rock.png",
          "several label terms must all be carried");
  t.check(search("l:env tree") == "tree.png",
          "the rest of the query still searches names");
  t.check(search("rock") == "rock.png rocket.lua",
          "a query without label terms searches names as before");
  t.check(search("l:nope").empty() &&
              search("l:abcdefghijklmnopqrstuvwxyz012345").empty(),
          "a label nothing carries, or none could, matches nothing");
  t.check(search("L:Rock") == "rock.png", "the term's prefix takes any case");
}

void check_save(engine::tests::TestContext &t) noexcept {
  const AssetIndexEntry *source = entry_named("hero.gltf");
  if (source == nullptr) {
    t.fail("the source is indexed");
    return;
  }
  const std::string sourcePath = source->osPath;
  const std::string virtualPath = source->virtualPath;

  // The catalog knows the source, with a tag a loader gave it.
  std::unique_ptr<ct::AssetCatalog> catalog(new (std::nothrow)
                                                ct::AssetCatalog());
  if (catalog == nullptr) {
    t.fail("the catalog could be allocated");
    return;
  }
  ct::AssetMetadata record{};
  record.assetId = ct::make_asset_id_from_path(virtualPath.c_str());
  record.typeTag = ct::AssetTypeTag::Mesh;
  ct::write_metadata_path(&record.filePath, virtualPath.c_str());
  t.check(ct::asset_metadata_add_tag(&record, "loader") &&
              ct::register_asset_metadata(catalog.get(), record),
          "the catalog holds the source");
  engine::runtime::EngineAssetDatabaseService service{};
  service.catalog = catalog.get();
  engine::runtime::set_editor_asset_service(&service);

  AssetFilterCache cache{};
  AssetFilterState filter{};
  std::snprintf(filter.query, sizeof(filter.query), "%s", "l:hero");
  static_cast<void>(refresh_asset_filter_cache(filter, &cache));
  const bool noneBefore = cache.matches.empty();

  ct::AssetLabels labels{};
  t.check(ct::asset_labels_add(&labels, "hero") &&
              ct::asset_labels_add(&labels, "character") &&
              save_asset_labels(sourcePath.c_str(), labels),
          "labels are saved");
  ct::AssetSidecar read{};
  t.check((ct::read_asset_sidecar(sourcePath.c_str(), &read) ==
           ct::SidecarReadResult::Ok) &&
              (read.labels == labels) && read.hasMeshImport &&
              (read.meshImport.scaleFactor == 3.0F),
          "the sidecar holds them and keeps its import settings");
  t.check(noneBefore && refresh_asset_filter_cache(filter, &cache) &&
              (cache.matches.size() == 1U) &&
              (std::strcmp(asset_index_entry(cache.matches[0])->name,
                           "hero.gltf") == 0),
          "the search sees the new label at once");
  t.check(ct::asset_has_tag(catalog.get(), record.assetId, "hero") &&
              ct::asset_has_tag(catalog.get(), record.assetId, "character") &&
              ct::asset_has_tag(catalog.get(), record.assetId, "loader"),
          "the catalog's tags gain the labels beside its own tag");

  ct::AssetLabels fewer{};
  t.check(ct::asset_labels_add(&fewer, "character") &&
              save_asset_labels(sourcePath.c_str(), fewer) &&
              !ct::asset_has_tag(catalog.get(), record.assetId, "hero") &&
              ct::asset_has_tag(catalog.get(), record.assetId, "character") &&
              ct::asset_has_tag(catalog.get(), record.assetId, "loader"),
          "a removed label leaves the catalog's tags, and nothing else does");
  engine::runtime::set_editor_asset_service(nullptr);

  const AssetIndexEntry *mesh = entry_named("hero.mesh");
  std::error_code ec{};
  t.check((mesh != nullptr) && !save_asset_labels(mesh->osPath, labels) &&
              !std::filesystem::exists(std::string(mesh->osPath) + ".meta", ec),
          "an asset with no sidecar is refused and none is created");
}

} // namespace

int main() {
  // The index walks the relative "assets" root; a private working
  // directory keeps it to this suite's own files under ctest -j.
  constexpr const char *kWorkDir = "engine_asset_labels_test_wd";
  std::error_code ec{};
  const std::filesystem::path previous = std::filesystem::current_path(ec);
  std::filesystem::remove_all(kWorkDir, ec);
  std::filesystem::create_directories(kWorkDir, ec);
  std::filesystem::current_path(kWorkDir, ec);
  if (ec || !engine::core::initialize_logging()) {
    return 2;
  }
  engine::tests::TestContext t;
  check_walk_and_search(t);
  check_save(t);
  engine::core::shutdown_logging();
  std::filesystem::current_path(previous, ec);
  std::filesystem::remove_all(kWorkDir, ec);
  return t.finish("editor_asset_labels");
}
