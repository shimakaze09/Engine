// Verifies the references documents make and the catalog's index of them:
// - the scanner visits every string value at any depth, never a key, as a
//   ref when it is AssetRef text and as text otherwise, and refuses text
//   that is not JSON;
// - a document's edges are replaced whole, deduplicated, and not capped at
//   the 32 a record's own dependency list holds;
// - indexing a mounted tree records each AssetRef that names a catalogued
//   asset and each string that is a catalogued path, counts the rest as
//   dangling, and logs a document that will not read;
// - dependents, change notification and the dependency closure follow the
//   document edges, through a material to its texture.

#include "engine/content/asset_references.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#include "../test_harness.h"
#include "engine/content/asset_catalog.h"
#include "engine/content/asset_identity.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"

namespace {

namespace ct = engine::content;

engine::tests::TestContext g_tests;

constexpr const char *kRoot = "asset_references_test_root";

bool write_text(const std::string &path, const std::string &text) {
  std::error_code ec{};
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(),
                                      ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  file.close();
  return !file.fail();
}

/// Writes `relative` under the root with `text` and the identity `guid`.
bool author(const char *relative, const std::string &text, const char *guid) {
  const std::string path = std::string(kRoot) + "/" + relative;
  ct::AssetSidecar sidecar{};
  return write_text(path, text) && ct::parse_asset_guid(guid, &sidecar.guid) &&
         ct::write_asset_sidecar(path.c_str(), sidecar);
}

struct Collected final {
  std::vector<std::string> refs;
  std::vector<std::string> texts;
};

void collect(const ct::DocumentReference &reference, void *userData) noexcept {
  auto &out = *static_cast<Collected *>(userData);
  if (reference.isRef) {
    char text[ct::kAssetRefTextLength + 1U] = {};
    static_cast<void>(ct::format_asset_ref(reference.ref, text, sizeof(text)));
    out.refs.emplace_back(text);
  } else {
    out.texts.emplace_back(reference.text);
  }
}

void check_scanner() {
  const std::string doc =
      "{\"aaaaaaaa-0000-4000-8000-000000000001\": 1,"
      " \"mesh\": \"aaaaaaaa-0000-4000-8000-000000000002#0000000000000007\","
      " \"list\": [\"assets/a.lua\", {\"deep\": [\"AAAAAAAA-0000-4000-8000-"
      "000000000003\"]}], \"n\": 3, \"b\": true, \"name\": \"Camera\"}";
  Collected found{};
  g_tests.check(
      ct::scan_document_references(doc.data(), doc.size(), &collect, &found),
      "a JSON document scans");
  g_tests.check((found.refs.size() == 2U) &&
                    (found.refs[0] ==
                     "aaaaaaaa-0000-4000-8000-000000000002#0000000000000007") &&
                    (found.refs[1] == "aaaaaaaa-0000-4000-8000-000000000003"),
                "refs at any depth are read, in any letter case, and a key "
                "is never one");
  g_tests.check((found.texts.size() == 2U) &&
                    (found.texts[0] == "assets/a.lua") &&
                    (found.texts[1] == "Camera"),
                "every other string value is visited as text");
  Collected none{};
  g_tests.check(
      !ct::scan_document_references("{\"open\": ", 9U, &collect, &none) &&
          none.refs.empty() && none.texts.empty(),
      "text that is not JSON is refused with nothing visited");
}

std::unique_ptr<ct::AssetCatalog> make_catalog() {
  return std::unique_ptr<ct::AssetCatalog>(new (std::nothrow)
                                               ct::AssetCatalog());
}

void check_table() {
  const auto catalog = make_catalog();
  if (catalog == nullptr) {
    g_tests.fail("allocate a catalog");
    return;
  }
  std::vector<ct::AssetId> targets{};
  for (ct::AssetId id = 1000U; id < 1100U; ++id) {
    targets.push_back(id);
  }
  targets.push_back(1000U);
  targets.push_back(ct::kInvalidAssetId);
  g_tests.check(
      ct::set_document_references(catalog.get(), 7U, targets.data(),
                                  targets.size()) &&
          (ct::get_document_references(catalog.get(), 7U, nullptr, 0U) == 100U),
      "a document keeps 100 distinct references: no 32-entry cap, "
      "duplicates and invalid ids dropped");
  const ct::AssetId other[] = {1000U, 5U};
  g_tests.check(
      ct::set_document_references(catalog.get(), 8U, other, 2U) &&
          (ct::find_asset_referrers(catalog.get(), 1000U, nullptr, 0U) == 2U),
      "two documents referencing one asset are both its referrers");
  const ct::AssetId replaced[] = {5U};
  ct::AssetId out[4] = {};
  g_tests.check(
      ct::set_document_references(catalog.get(), 7U, replaced, 1U) &&
          (ct::get_document_references(catalog.get(), 7U, out, 4U) == 1U) &&
          (out[0] == 5U) &&
          (ct::find_asset_referrers(catalog.get(), 1000U, out, 4U) == 1U) &&
          (out[0] == 8U),
      "setting a document's references replaces them whole");
  ct::clear_asset_catalog(catalog.get());
  g_tests.check(ct::find_asset_referrers(catalog.get(), 5U, nullptr, 0U) == 0U,
                "clearing the catalog clears its references");
}

constexpr const char *kTextureGuid = "11111111-0000-4000-8000-000000000001";
constexpr const char *kMaterialGuid = "11111111-0000-4000-8000-000000000002";
constexpr const char *kSceneGuid = "11111111-0000-4000-8000-000000000003";
constexpr const char *kScriptGuid = "11111111-0000-4000-8000-000000000004";
constexpr const char *kBrokenGuid = "11111111-0000-4000-8000-000000000005";
constexpr const char *kBigGuid = "11111111-0000-4000-8000-000000000006";

std::size_t g_visits = 0U;
ct::AssetId g_lastDependent = ct::kInvalidAssetId;
void count_visit(ct::AssetId dependent, ct::AssetId, void *) {
  ++g_visits;
  g_lastDependent = dependent;
}

void check_index() {
  std::error_code ec{};
  std::filesystem::remove_all(kRoot, ec);
  // 40 sounds, so one scene names more assets than a record's list holds.
  std::string bigScene = "{\"version\": 6, \"sounds\": [";
  bool authored = true;
  for (int i = 0; i < 40; ++i) {
    char relative[64] = {};
    std::snprintf(relative, sizeof(relative), "sounds/s%02d.wav", i);
    char guid[40] = {};
    std::snprintf(guid, sizeof(guid), "22222222-0000-4000-8000-%012d", i);
    authored = authored && author(relative, "RIFF", guid);
    bigScene += std::string((i == 0) ? "" : ", ") + "\"" + guid + "\"";
  }
  bigScene += "]}";
  authored =
      authored && author("tex.png", "x", kTextureGuid) &&
      author("mat.mat",
             std::string("{\"version\": 4, \"textures\": {\"albedo\": \"") +
                 kTextureGuid + "\"}}",
             kMaterialGuid) &&
      author("game.lua", "-- script", kScriptGuid) &&
      author("level.scene",
             std::string("{\"version\": 6, \"entities\": [{\"components\": "
                         "{\"MeshComponent\": {\"material\": \"") +
                 kMaterialGuid +
                 "\"}, \"ScriptComponent\": \"assets/game.lua\", \"other\": "
                 "\"99999999-0000-4000-8000-000000000009\"}}]}",
             kSceneGuid) &&
      author("broken.scene", "{\"version\": 6, \"entities\": [", kBrokenGuid) &&
      author("big.scene", bigScene, kBigGuid);
  const auto catalog = make_catalog();
  if (!authored || (catalog == nullptr) || !engine::core::initialize_vfs() ||
      !engine::core::mount("assets", kRoot)) {
    g_tests.fail("author and mount the tree");
    return;
  }
  g_tests.check(ct::register_mounted_assets(catalog.get(), "assets", kRoot).ok,
                "the tree catalogues");
  const ct::DocumentIndexReport report =
      ct::index_catalogued_documents(catalog.get());
  g_tests.check((report.documents == 3U) && (report.failed == 1U) &&
                    (report.dangling == 1U) &&
                    (report.references == 1U + 2U + 40U),
                "three documents index, the broken one is counted, and the "
                "ref to nothing is dangling");

  const ct::AssetId tex = ct::make_asset_id_from_path("assets/tex.png");
  const ct::AssetId mat = ct::make_asset_id_from_path("assets/mat.mat");
  const ct::AssetId level = ct::make_asset_id_from_path("assets/level.scene");
  const ct::AssetId script = ct::make_asset_id_from_path("assets/game.lua");
  const ct::AssetId big = ct::make_asset_id_from_path("assets/big.scene");
  ct::AssetId ids[64] = {};
  g_tests.check(
      (ct::get_document_references(catalog.get(), level, ids, 64U) == 2U) &&
          (((ids[0] == mat) && (ids[1] == script)) ||
           ((ids[0] == script) && (ids[1] == mat))),
      "the scene references its material by ref and its script by "
      "path");
  g_tests.check(ct::get_document_references(catalog.get(), big, nullptr, 0U) ==
                    40U,
                "a scene naming 40 assets keeps all 40");
  g_tests.check(
      (ct::find_asset_dependents(catalog.get(), mat, ids, 64U) == 1U) &&
          (ids[0] == level),
      "the scene is a dependent of the material it references");

  g_visits = 0U;
  g_tests.check((ct::notify_asset_changed(catalog.get(), tex, &count_visit,
                                          nullptr) == 2U) &&
                    (g_visits == 2U) && (g_lastDependent == level),
                "a texture change reaches its material and, through it, the "
                "scene");
  const std::size_t closure =
      ct::collect_asset_closure(catalog.get(), level, ids, 64U);
  bool hasTex = false;
  bool hasMat = false;
  bool hasScript = false;
  for (std::size_t i = 0U; i < closure; ++i) {
    hasTex = hasTex || (ids[i] == tex);
    hasMat = hasMat || (ids[i] == mat);
    hasScript = hasScript || (ids[i] == script);
  }
  g_tests.check((closure == 3U) && hasTex && hasMat && hasScript,
                "the scene's closure is its material, the material's texture "
                "and its script");

  // The scene now names only the script: re-indexing it replaces its edges.
  const bool rewritten =
      write_text(std::string(kRoot) + "/level.scene",
                 "{\"version\": 6, \"entities\": [{\"components\": "
                 "{\"ScriptComponent\": \"assets/game.lua\"}}]}");
  const ct::DocumentIndexReport again =
      ct::index_document_references(catalog.get(), level);
  g_tests.check(
      rewritten && (again.documents == 1U) &&
          (ct::get_document_references(catalog.get(), level, ids, 64U) == 1U) &&
          (ids[0] == script) &&
          (ct::find_asset_dependents(catalog.get(), mat, nullptr, 0U) == 0U),
      "re-indexing a saved scene replaces its references");
  engine::core::shutdown_vfs();
  std::filesystem::remove_all(kRoot, ec);
}

} // namespace

/// Runs the asset references suite.
int main() {
  static_cast<void>(engine::core::initialize_logging());
  check_scanner();
  check_table();
  check_index();
  engine::core::shutdown_logging();
  return g_tests.finish("asset references");
}
