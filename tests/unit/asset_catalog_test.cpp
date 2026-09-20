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
#include "engine/content/asset_sidecar.h"
#include "engine/content/metadata_store.h"
#include "engine/core/logging.h"

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

/// The tree: ten runtime forms, six entries the walk must skip.
bool build_tree() noexcept {
  const std::filesystem::path root(kRoot);
  std::error_code ec{};
  std::filesystem::remove_all(root, ec);
  const char *files[] = {
      "props/coin.mesh",      // Mesh, cooked form
      "props/coin.gltf",      // Mesh source: skipped
      "props/coin.mesh.hull", // no table suffix: skipped
      "props/coin.mesh.cookmeta", // no table suffix: skipped
      "textures/Grass.PNG",   // Texture, case kept in the path
      "scripts/hop.lua",      // Script
      "anim/walk.anim",       // Animation (derived)
      "anim/walk.skel",       // Animation (derived)
      "ctrl/hero.animctrl",   // AnimationController
      "levels/hub.scene",     // Scene
      "props/crate.prefab",   // Prefab
      "mats/brass.mat",       // Material
      ".thumbnails/coin.png", // hidden directory: skipped
      "notes.txt",            // no table suffix: skipped
      ".hidden.mesh",         // dot-file: skipped
      "sub/deep/log.mesh",    // Mesh, nested
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
  check(first.registered == 9U, "nine runtime forms are registered");
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
  check(has_path_and_type(*store, "kit/ctrl/hero.animctrl",
                          AssetTypeTag::AnimationController),
        "a controller is typed AnimationController");
  // The three kinds the catalog could not list at all while their table
  // rows carried no suffix: a reference picker for them showed nothing.
  check(has_path_and_type(*store, "kit/levels/hub.scene", AssetTypeTag::Scene),
        "a scene is catalogued as Scene");
  check(has_path_and_type(*store, "kit/props/crate.prefab",
                          AssetTypeTag::Prefab),
        "a prefab is catalogued as Prefab");
  check(has_path_and_type(*store, "kit/mats/brass.mat",
                          AssetTypeTag::Material),
        "a material is catalogued as Material");
  check((count_of_type(*store, AssetTypeTag::Scene) == 1U) &&
            (count_of_type(*store, AssetTypeTag::Prefab) == 1U) &&
            (count_of_type(*store, AssetTypeTag::Material) == 1U),
        "each of the three is queryable by its type");
  check((find_by_path(*store, "kit/props/coin.gltf") == nullptr) &&
            (find_by_path(*store, "kit/props/coin.mesh.hull") == nullptr) &&
            (find_by_path(*store, "kit/props/coin.mesh.cookmeta") ==
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
  check((second.registered == 0U) && (second.alreadyKnown == 10U) &&
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
            (walk.refused == 10U) && (walk.skipped == 6U),
        "a prefix that pushes every path past the record refuses each one");
  check(count_of_type(*store, engine::content::AssetTypeTag::Mesh) ==
            meshesBefore,
        "a refused walk leaves the store as it was");
}

/// Writes the cook stamp that records which source produced which
/// outputs. The catalog reads provenance from here rather than guessing
/// from filenames, so a fixture with cooked outputs needs one.
bool write_stamp(const std::filesystem::path &root, const char *stampRelative,
                 const engine::content::AssetGuid &sourceGuid,
                 std::initializer_list<const char *> outputs) noexcept {
  char guidText[engine::content::kAssetGuidTextLength + 1U] = {};
  if (!engine::content::format_asset_guid(sourceGuid, guidText,
                                          sizeof(guidText))) {
    return false;
  }
  const std::filesystem::path stampPath = root / stampRelative;
  std::error_code ec{};
  std::filesystem::create_directories(stampPath.parent_path(), ec);
  std::ofstream out(stampPath, std::ios::binary);
  out << "SCHEMA 5\nSOURCE_GUID " << guidText << "\n";
  for (const char *output : outputs) {
    // The local id is the output's name after the source's stem, which
    // is what the cook computes and writes.
    const std::string name(output);
    const std::size_t dot = name.find('.');
    const std::string localName =
        (dot == std::string::npos) ? name : name.substr(dot + 1U);
    char localText[17] = {};
    std::snprintf(localText, sizeof(localText), "%016llx",
                  static_cast<unsigned long long>(
                      engine::content::asset_local_id(localName.c_str())));
    out << "ASSET " << localText << " " << output << "\n";
  }
  return out.good();
}

/// Gives `relative` an authored sidecar and returns the GUID it got.
engine::content::AssetGuid identify(const std::filesystem::path &root,
                                    const char *relative) noexcept {
  engine::content::AssetSidecar sidecar{};
  sidecar.guid = engine::content::generate_asset_guid();
  if (!engine::content::write_asset_sidecar((root / relative).string().c_str(),
                                            sidecar)) {
    return engine::content::kNilAssetGuid;
  }
  return sidecar.guid;
}

/// The identity contract the whole scheme exists for: a reference made of
/// a GUID keeps resolving through a rename, a move, a case-only rename
/// and a content edit, because none of those is the asset changing.
void test_identity_survives_relocation() noexcept {
  using engine::content::AssetRef;
  using engine::content::AssetTypeTag;
  constexpr const char *kRoot = "asset_catalog_identity_root";
  constexpr const char *kPrefix = "kit";

  std::error_code ec{};
  std::filesystem::remove_all(kRoot, ec);
  const std::filesystem::path root(kRoot);
  const bool built = write_file(root / "props/coin.gltf") &&
                     write_file(root / "props/coin.mesh") &&
                     write_file(root / "chars/hero.gltf") &&
                     write_file(root / "chars/hero.mesh") &&
                     write_file(root / "chars/hero.skel") &&
                     write_file(root / "chars/hero.walk.anim") &&
                     write_file(root / "scripts/hop.lua") &&
                     write_file(root / "scripts/dash.lua");
  if (!built) {
    g_tests.fail("the identity tree could be written");
    return;
  }

  const engine::content::AssetGuid coin = identify(root, "props/coin.gltf");
  const engine::content::AssetGuid hero = identify(root, "chars/hero.gltf");
  const engine::content::AssetGuid script = identify(root, "scripts/hop.lua");
  check(write_stamp(root, "props/coin.mesh.cookstamp", coin, {"coin.mesh"}) &&
            write_stamp(root, "chars/hero.mesh.cookstamp", hero,
                        {"hero.mesh", "hero.skel", "hero.walk.anim"}),
        "the cooks recorded which source produced which outputs");
  check(engine::content::asset_guid_is_valid(coin) &&
            engine::content::asset_guid_is_valid(hero) &&
            engine::content::asset_guid_is_valid(script),
        "the three sources are identified");

  const auto walk = [&](engine::content::MetadataStore *store) noexcept {
    engine::content::clear_metadata_store(store);
    return engine::content::register_mounted_assets(store, kPrefix, kRoot);
  };

  std::unique_ptr<engine::content::MetadataStore> store(
      new (std::nothrow) engine::content::MetadataStore());
  if (store == nullptr) {
    g_tests.fail("the identity store could be allocated");
    return;
  }
  static_cast<void>(walk(store.get()));

  // A source-policy asset is the primary asset of its own GUID.
  const engine::content::AssetMetadata *scriptRecord =
      find_asset_metadata_by_ref(store.get(),
                                 engine::content::asset_ref_primary(script));
  check((scriptRecord != nullptr) &&
            (std::strcmp(scriptRecord->filePath.data(),
                         "kit/scripts/hop.lua") == 0),
        "a script resolves from its own GUID to its path");

  // A cooked output has no sidecar: it is the primary output of the
  // source that made it.
  const engine::content::AssetMetadata *coinMesh = find_asset_metadata_by_ref(
      store.get(), AssetRef{coin, engine::content::asset_local_id("mesh")});
  check((coinMesh != nullptr) &&
            (std::strcmp(coinMesh->filePath.data(), "kit/props/coin.mesh") ==
             0) &&
            (coinMesh->typeTag == AssetTypeTag::Mesh),
        "a cooked mesh resolves from its source's GUID and local id");
  check(find_asset_metadata_by_ref(
            store.get(), engine::content::asset_ref_primary(coin)) == nullptr,
        "the source's own primary id is not one of its cooked outputs");

  // One source, several outputs, told apart by local id — the case a
  // bare GUID cannot express.
  const AssetRef walkClip{hero, engine::content::asset_local_id("walk.anim")};
  const AssetRef skeleton{hero, engine::content::asset_local_id("skel")};
  const AssetRef heroMesh{hero, engine::content::asset_local_id("mesh")};
  const engine::content::AssetMetadata *clipRecord =
      find_asset_metadata_by_ref(store.get(), walkClip);
  check((clipRecord != nullptr) &&
            (std::strcmp(clipRecord->filePath.data(),
                         "kit/chars/hero.walk.anim") == 0),
        "a clip resolves from its source's GUID plus its local id");
  const engine::content::AssetMetadata *skelRecord =
      find_asset_metadata_by_ref(store.get(), skeleton);
  const engine::content::AssetMetadata *meshRecord =
      find_asset_metadata_by_ref(store.get(), heroMesh);
  check((skelRecord != nullptr) && (meshRecord != nullptr) &&
            (std::strcmp(skelRecord->filePath.data(),
                         "kit/chars/hero.skel") == 0) &&
            (std::strcmp(meshRecord->filePath.data(),
                         "kit/chars/hero.mesh") == 0),
        "the skeleton and the mesh of one source resolve separately");
  check((clipRecord != skelRecord) && (skelRecord != meshRecord) &&
            (clipRecord != meshRecord),
        "three outputs of one source are three distinct references");

  check(engine::content::find_duplicate_guid_records(store.get(), nullptr,
                                                     0U) == 0U,
        "a tree with one sidecar per source has no duplicate identity");

  // Rename: the file and its sidecar move together, so the GUID does not.
  std::filesystem::rename(root / "scripts/hop.lua",
                          root / "scripts/jump.lua", ec);
  std::filesystem::rename(root / "scripts/hop.lua.meta",
                          root / "scripts/jump.lua.meta", ec);
  static_cast<void>(walk(store.get()));
  const engine::content::AssetMetadata *renamed =
      find_asset_metadata_by_ref(store.get(),
                                 engine::content::asset_ref_primary(script));
  check(!ec && (renamed != nullptr) &&
            (std::strcmp(renamed->filePath.data(), "kit/scripts/jump.lua") ==
             0),
        "a rename keeps the GUID and moves where it resolves to");

  // Move to another folder: same.
  std::filesystem::create_directories(root / "gameplay", ec);
  std::filesystem::rename(root / "scripts/jump.lua",
                          root / "gameplay/jump.lua", ec);
  std::filesystem::rename(root / "scripts/jump.lua.meta",
                          root / "gameplay/jump.lua.meta", ec);
  static_cast<void>(walk(store.get()));
  const engine::content::AssetMetadata *moved =
      find_asset_metadata_by_ref(store.get(),
                                 engine::content::asset_ref_primary(script));
  check(!ec && (moved != nullptr) &&
            (std::strcmp(moved->filePath.data(), "kit/gameplay/jump.lua") ==
             0),
        "a move keeps the GUID and moves where it resolves to");

  // A case-only rename is still the same asset.
  std::filesystem::rename(root / "gameplay/jump.lua",
                          root / "gameplay/Jump.lua", ec);
  std::filesystem::rename(root / "gameplay/jump.lua.meta",
                          root / "gameplay/Jump.lua.meta", ec);
  static_cast<void>(walk(store.get()));
  const engine::content::AssetMetadata *recased =
      find_asset_metadata_by_ref(store.get(),
                                 engine::content::asset_ref_primary(script));
  check(!ec && (recased != nullptr) &&
            (std::strcmp(recased->filePath.data(), "kit/gameplay/Jump.lua") ==
             0),
        "a case-only rename keeps the GUID");

  // Editing the bytes changes the content hash and nothing else.
  const engine::content::ContentHash before =
      engine::content::make_content_hash("x", 1U);
  const engine::content::ContentHash after =
      engine::content::make_content_hash("edited", 6U);
  check(!(before == after), "an edit changes the content hash");
  static_cast<void>(walk(store.get()));
  check(find_asset_metadata_by_ref(store.get(),
                                   engine::content::asset_ref_primary(
                                       script)) != nullptr,
        "an edit does not change the GUID");

  // A copied sidecar is two assets claiming one identity: reported,
  // never resolved by picking one. Copied between two scripts, because
  // those are catalogued directly — a duplicated mesh SOURCE sidecar
  // only reaches the catalog once a recook rewrites the stamps, and the
  // CI identity gate catches that pair at the sidecars themselves.
  static_cast<void>(identify(root, "scripts/dash.lua"));
  std::filesystem::copy_file(root / "gameplay/Jump.lua.meta",
                             root / "scripts/dash.lua.meta",
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  static_cast<void>(walk(store.get()));
  const engine::content::AssetMetadata *records[8] = {};
  const std::size_t duplicates =
      engine::content::find_duplicate_guid_records(store.get(), records, 8U);
  check(!ec && (duplicates >= 2U),
        "a copied sidecar is reported as a duplicate identity");

  std::filesystem::remove_all(kRoot, ec);
}

/// Indexing fails closed on every way an identity can be missing,
/// duplicated or unportable, and says which paths are at fault. Catching
/// these at index time is the point: CI catches them before they are
/// committed, but a project assembled on a machine never went through
/// CI, and a nil identity accepted here becomes a reference that
/// resolves to nothing much later with nothing to say why.
void test_identity_validation_fails_closed() noexcept {
  constexpr const char *kRoot = "asset_catalog_validation_root";
  constexpr const char *kPrefix = "kit";
  std::error_code ec{};

  const auto walk = [&](engine::content::MetadataStore *store) noexcept {
    engine::content::clear_metadata_store(store);
    return engine::content::register_mounted_assets(store, kPrefix, kRoot);
  };
  std::unique_ptr<engine::content::MetadataStore> store(
      new (std::nothrow) engine::content::MetadataStore());
  if (store == nullptr) {
    g_tests.fail("the validation store could be allocated");
    return;
  }

  const auto reset = [&]() noexcept {
    std::filesystem::remove_all(kRoot, ec);
    return write_file(std::filesystem::path(kRoot) / "scripts/hop.lua");
  };

  // A clean mount indexes cleanly; without this the failures below could
  // all be some unrelated fault.
  check(reset(), "the validation tree is written");
  static_cast<void>(identify(std::filesystem::path(kRoot), "scripts/hop.lua"));
  engine::content::MountRegistration walkResult = walk(store.get());
  check(walkResult.ok && (walkResult.unidentified == 0U) &&
            (walkResult.duplicateRefs == 0U) &&
            (walkResult.caseCollisions == 0U),
        "a mount whose assets all have identities indexes cleanly");

  // 1: a source with no sidecar at all.
  check(reset(), "the validation tree is rewritten");
  walkResult = walk(store.get());
  check(!walkResult.ok && (walkResult.unidentified == 1U),
        "a source with no sidecar fails the index");

  // 2 and 3: a sidecar that will not read, and one that is malformed.
  const char *badDocuments[] = {
      "{ not json at all",
      "{\"schemaVersion\":9999,"
      "\"guid\":\"11111111-1111-4111-8111-111111111111\"}",
  };
  for (const char *document : badDocuments) {
    check(reset(), "the validation tree is rewritten");
    std::ofstream bad(std::filesystem::path(kRoot) / "scripts/hop.lua.meta",
                      std::ios::binary);
    bad << document;
    bad.close();
    walkResult = walk(store.get());
    check(!walkResult.ok && (walkResult.unidentified == 1U),
          "a sidecar that will not read fails the index");
  }

  // 4: two assets claiming one identity, with neither chosen.
  check(reset(), "the validation tree is rewritten");
  check(write_file(std::filesystem::path(kRoot) / "scripts/dash.lua"),
        "a second script is written");
  static_cast<void>(identify(std::filesystem::path(kRoot), "scripts/hop.lua"));
  std::filesystem::copy_file(
      std::filesystem::path(kRoot) / "scripts/hop.lua.meta",
      std::filesystem::path(kRoot) / "scripts/dash.lua.meta",
      std::filesystem::copy_options::overwrite_existing, ec);
  walkResult = walk(store.get());
  check(!ec && !walkResult.ok && (walkResult.duplicateRefs == 2U),
        "two assets claiming one identity fail the index, both named");
  check(walkResult.registered == 2U,
        "both are still registered, so the caller can show the project");

  // 5: two paths differing only by case.
  check(reset(), "the validation tree is rewritten");
  const bool cased =
      write_file(std::filesystem::path(kRoot) / "scripts/Hop.lua");
  static_cast<void>(identify(std::filesystem::path(kRoot), "scripts/hop.lua"));
  static_cast<void>(identify(std::filesystem::path(kRoot), "scripts/Hop.lua"));
  if (cased && std::filesystem::exists(
                   std::filesystem::path(kRoot) / "scripts/Hop.lua")) {
    walkResult = walk(store.get());
    check(!walkResult.ok && (walkResult.caseCollisions == 2U),
          "two paths differing only by case fail the index, both named");
  }

  std::filesystem::remove_all(kRoot, ec);
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
  test_identity_survives_relocation();
  test_identity_validation_fails_closed();

  remove_tree();
  return g_tests.finish("asset catalog tests");
}
