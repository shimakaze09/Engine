// Pins the mount walk behind content::register_mounted_assets on a
// temporary tree: only runtime asset forms are registered, under the
// mount prefix plus the generic relative path, typed by the asset type
// table; hidden entries and non-asset files are skipped; a record already
// in the store is kept whole; a second walk registers nothing; a prefix
// that pushes a path past the record refuses the file and leaves the
// store untouched; null arguments and a missing root do nothing.

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "engine/content/asset_catalog.h"
#include "engine/content/metadata_store.h"

namespace {

engine::tests::TestContext g_tests;

constexpr const char *kRoot = "asset_catalog_test_root";
constexpr const char *kPrefix = "kit";

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

bool write_file(const std::filesystem::path &path) noexcept {
  std::error_code ec{};
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  out << "x";
  return out.good();
}

/// The tree: seven runtime forms, six entries the walk must skip.
bool build_tree() noexcept {
  const std::filesystem::path root(kRoot);
  std::error_code ec{};
  std::filesystem::remove_all(root, ec);
  const char *files[] = {
      "props/coin.mesh",          // Mesh, cooked form
      "props/coin.gltf",          // Mesh source: skipped
      "props/coin.mesh.hull",     // no table suffix: skipped
      "props/coin.mesh.meta.json", // no table suffix: skipped
      "textures/Grass.PNG",       // Texture, case kept in the path
      "scripts/hop.lua",          // Script
      "anim/walk.anim",           // Animation (derived)
      "anim/walk.skel",           // Animation (derived)
      "ctrl/hero.animctrl.json",  // AnimationController
      ".thumbnails/coin.png",     // hidden directory: skipped
      "notes.txt",                // no table suffix: skipped
      ".hidden.mesh",             // dot-file: skipped
      "sub/deep/log.mesh",        // Mesh, nested
  };
  for (const char *file : files) {
    if (!write_file(root / file)) {
      return false;
    }
  }
  return true;
}

void remove_tree() noexcept {
  std::error_code ec{};
  std::filesystem::remove_all(kRoot, ec);
}

const engine::content::AssetMetadata *
find_by_path(const engine::content::MetadataStore &store,
             const char *virtualPath) noexcept {
  return engine::content::find_asset_metadata(
      &store, engine::content::make_asset_id_from_path(virtualPath));
}

bool has_path_and_type(const engine::content::MetadataStore &store,
                       const char *virtualPath,
                       engine::content::AssetTypeTag tag) noexcept {
  const engine::content::AssetMetadata *metadata =
      find_by_path(store, virtualPath);
  return (metadata != nullptr) && (metadata->typeTag == tag) &&
         (std::strcmp(metadata->filePath.data(), virtualPath) == 0);
}

std::size_t count_of_type(const engine::content::MetadataStore &store,
                          engine::content::AssetTypeTag tag) noexcept {
  engine::content::AssetId ids[64] = {};
  return engine::content::query_assets_by_type(&store, tag, ids, 64U);
}

void test_invalid_arguments(engine::content::MetadataStore *store) noexcept {
  using engine::content::MountRegistration;
  using engine::content::register_mounted_assets;
  const MountRegistration nullStore =
      register_mounted_assets(nullptr, kPrefix, kRoot);
  const MountRegistration nullPrefix =
      register_mounted_assets(store, nullptr, kRoot);
  const MountRegistration emptyPrefix =
      register_mounted_assets(store, "", kRoot);
  const MountRegistration nullRoot =
      register_mounted_assets(store, kPrefix, nullptr);
  const MountRegistration missingRoot =
      register_mounted_assets(store, kPrefix, "asset_catalog_test_absent");
  const auto zero = [](const MountRegistration &r) noexcept {
    return (r.registered == 0U) && (r.alreadyKnown == 0U) &&
           (r.skipped == 0U) && (r.refused == 0U);
  };
  check(zero(nullStore) && zero(nullPrefix) && zero(emptyPrefix) &&
            zero(nullRoot) && zero(missingRoot),
        "null arguments and a missing root register nothing");
  check(count_of_type(*store, engine::content::AssetTypeTag::Mesh) == 0U,
        "an invalid walk leaves the store empty");
}

void test_walk(engine::content::MetadataStore *store) noexcept {
  using engine::content::AssetTypeTag;

  // A loader registered this mesh first, with a tag the walk must keep.
  engine::content::AssetMetadata authored{};
  authored.assetId =
      engine::content::make_asset_id_from_path("kit/props/coin.mesh");
  authored.typeTag = AssetTypeTag::Mesh;
  engine::content::write_metadata_path(&authored.filePath,
                                       "kit/props/coin.mesh");
  check(engine::content::asset_metadata_add_tag(&authored, "authored") &&
            engine::content::register_asset_metadata(store, authored),
        "the pre-registered record is in place");

  const engine::content::MountRegistration first =
      engine::content::register_mounted_assets(store, kPrefix, kRoot);
  check(first.registered == 6U, "six runtime forms are registered");
  check(first.alreadyKnown == 1U, "the pre-registered mesh is already known");
  check(first.skipped == 6U, "six entries are skipped");
  check(first.refused == 0U, "nothing is refused");

  check(has_path_and_type(*store, "kit/props/coin.mesh", AssetTypeTag::Mesh),
        "coin.mesh is a Mesh under the mount prefix");
  check(engine::content::asset_has_tag(store, authored.assetId, "authored"),
        "the pre-registered record keeps its tag");
  check(has_path_and_type(*store, "kit/sub/deep/log.mesh",
                          AssetTypeTag::Mesh),
        "a nested mesh keeps its full relative path");
  check(has_path_and_type(*store, "kit/textures/Grass.PNG",
                          AssetTypeTag::Texture),
        "a texture is typed Texture with its path's case kept");
  check(has_path_and_type(*store, "kit/scripts/hop.lua", AssetTypeTag::Script),
        "a script is typed Script");
  check(has_path_and_type(*store, "kit/anim/walk.anim",
                          AssetTypeTag::Animation) &&
            has_path_and_type(*store, "kit/anim/walk.skel",
                              AssetTypeTag::Animation),
        "derived animation outputs are typed Animation");
  check(has_path_and_type(*store, "kit/ctrl/hero.animctrl.json",
                          AssetTypeTag::AnimationController),
        "a controller is typed AnimationController");
  check((find_by_path(*store, "kit/props/coin.gltf") == nullptr) &&
            (find_by_path(*store, "kit/props/coin.mesh.hull") == nullptr) &&
            (find_by_path(*store, "kit/props/coin.mesh.meta.json") ==
             nullptr) &&
            (find_by_path(*store, "kit/notes.txt") == nullptr),
        "a cooked type's source, sidecars and unclassified files are absent");
  check((find_by_path(*store, "kit/.thumbnails/coin.png") == nullptr) &&
            (find_by_path(*store, "kit/.hidden.mesh") == nullptr),
        "hidden entries are absent");
  check(count_of_type(*store, AssetTypeTag::Mesh) == 2U,
        "exactly two meshes are catalogued");
  check(count_of_type(*store, AssetTypeTag::Unknown) == 0U,
        "nothing is catalogued as Unknown");

  const engine::content::MountRegistration second =
      engine::content::register_mounted_assets(store, kPrefix, kRoot);
  check((second.registered == 0U) && (second.alreadyKnown == 7U) &&
            (second.skipped == 6U) && (second.refused == 0U),
        "a second walk registers nothing and knows every runtime form");
}

void test_overlong_prefix(engine::content::MetadataStore *store) noexcept {
  // A prefix that leaves no room for any relative path: every runtime
  // form is refused and the store keeps what it had.
  std::string prefix(engine::content::AssetMetadata{}.filePath.size() - 4U,
                     'p');
  const std::size_t meshesBefore =
      count_of_type(*store, engine::content::AssetTypeTag::Mesh);
  const engine::content::MountRegistration walk =
      engine::content::register_mounted_assets(store, prefix.c_str(), kRoot);
  check((walk.registered == 0U) && (walk.alreadyKnown == 0U) &&
            (walk.refused == 7U) && (walk.skipped == 6U),
        "a prefix that pushes every path past the record refuses each one");
  check(count_of_type(*store, engine::content::AssetTypeTag::Mesh) ==
            meshesBefore,
        "a refused walk leaves the store as it was");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!build_tree()) {
    g_tests.fail("the temporary asset tree could be written");
    remove_tree();
    return g_tests.finish("asset catalog tests");
  }

  // On the heap: the store is 4 MB of fixed slots, which overflows the
  // 1 MB stack a Windows thread gets by default.
  std::unique_ptr<engine::content::MetadataStore> store(
      new (std::nothrow) engine::content::MetadataStore());
  if (store == nullptr) {
    g_tests.fail("the metadata store could be allocated");
    remove_tree();
    return g_tests.finish("asset catalog tests");
  }
  test_invalid_arguments(store.get());
  test_walk(store.get());
  test_overlong_prefix(store.get());

  remove_tree();
  return g_tests.finish("asset catalog tests");
}
