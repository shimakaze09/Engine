// Verifies reload_material_asset: a good reload replaces the in-memory
// record in place, and a malformed reload leaves the previous valid state
// completely untouched (editor hot-reload safety contract, issue #160).

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "engine/content/asset_catalog.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"

#include "../material_ref_fixture.h"

namespace {

/// The engine asset catalog the material API resolves through; one per
/// run, cleared wherever the database is.
engine::content::AssetCatalog *g_catalog = nullptr;

bool exactly_equal(float lhs, float rhs) noexcept { return lhs == rhs; }

bool write_material_file(const char *path, const char *text) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t size = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

void remove_file(const char *path) noexcept {
  static_cast<void>(std::remove(path));
}

/// A successful reload fully replaces the resolved values.
int verify_reload_success(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_reload_ok.json";
  constexpr const char *kVirtualPath = "mat/material_reload_ok.json";

  if (!write_material_file(kPath, "{\"version\":4,\"roughness\":0.2}")) {
    return 10;
  }
  const auto loadResult =
      engine::renderer::load_material_asset(database, g_catalog, kVirtualPath);
  if (!loadResult.has_value()) {
    remove_file(kPath);
    return 11;
  }
  const engine::content::AssetId id = *loadResult;

  char reloadJson[192] = {};
  std::snprintf(
      reloadJson, sizeof(reloadJson),
      "{\"version\":4,\"roughness\":0.8,\"metallic\":1.0,"
      "\"textures\":{\"albedo\":\"%s\"}}",
      engine::tests::catalog_texture(g_catalog, "assets/textures/new.png")
          .text);
  if (!write_material_file(kPath, reloadJson)) {
    remove_file(kPath);
    return 12;
  }
  const std::uint32_t generationBefore =
      engine::content::asset_reload_generation(g_catalog, id);
  const auto reloadResult = engine::renderer::reload_material_asset(
      database, g_catalog, kVirtualPath);
  remove_file(kPath);
  if (!reloadResult.has_value() || (*reloadResult != id)) {
    return 13;
  }
  // #682: a committed reload moves the catalog's reload generation once.
  if (engine::content::asset_reload_generation(g_catalog, id) !=
      generationBefore + 1U) {
    return 16;
  }

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  if ((params == nullptr) || !exactly_equal(params->roughness, 0.8F) ||
      !exactly_equal(params->metallic, 1.0F)) {
    return 14;
  }

  const engine::renderer::MaterialTextureSlots *slots =
      engine::renderer::find_material_texture_slots(database, id);
  const engine::content::AssetId expectedAlbedo =
      engine::content::make_asset_id_from_path("assets/textures/new.png");
  if ((slots == nullptr) || (slots->albedo != expectedAlbedo)) {
    return 15;
  }

  return 0;
}

/// A malformed reload leaves the previous, still-valid record untouched.
int verify_reload_malformed_preserves_previous(
    engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_reload_bad.json";
  constexpr const char *kVirtualPath = "mat/material_reload_bad.json";

  if (!write_material_file(kPath, "{\"version\":4,\"roughness\":0.33,"
                                  "\"metallic\":0.11}")) {
    return 20;
  }
  const auto loadResult =
      engine::renderer::load_material_asset(database, g_catalog, kVirtualPath);
  if (!loadResult.has_value()) {
    remove_file(kPath);
    return 21;
  }
  const engine::content::AssetId id = *loadResult;

  // Corrupt the file on disk (malformed JSON) and attempt a reload.
  if (!write_material_file(kPath, "{ this is not valid json")) {
    remove_file(kPath);
    return 22;
  }
  const std::uint32_t generationBefore =
      engine::content::asset_reload_generation(g_catalog, id);
  const auto reloadResult = engine::renderer::reload_material_asset(
      database, g_catalog, kVirtualPath);
  remove_file(kPath);
  if (reloadResult.has_value() ||
      (reloadResult.error() != engine::renderer::MaterialLoadError::Parse)) {
    return 23;
  }
  if (engine::content::asset_reload_generation(g_catalog, id) !=
      generationBefore) {
    return 26;
  }

  // Previous valid values are exactly as they were before the bad reload.
  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  if ((params == nullptr) || !exactly_equal(params->roughness, 0.33F) ||
      !exactly_equal(params->metallic, 0.11F)) {
    return 24;
  }
  if (engine::renderer::material_asset_state(database, id) !=
      engine::content::AssetState::Ready) {
    return 25;
  }

  return 0;
}

/// The material tables and the catalog as raw bytes: what a failed reload
/// must leave exactly as it found.
struct LiveStateBytes final {
  std::vector<unsigned char> bytes;
};

void append_bytes(std::vector<unsigned char> *out, const void *data,
                  std::size_t size) {
  const auto *begin = static_cast<const unsigned char *>(data);
  out->insert(out->end(), begin, begin + size);
}

LiveStateBytes live_state(const engine::renderer::AssetDatabase &database) {
  LiveStateBytes state{};
  append_bytes(&state.bytes, &database.materialAssets,
               sizeof(database.materialAssets));
  append_bytes(&state.bytes, &database.materialOccupied,
               sizeof(database.materialOccupied));
  append_bytes(&state.bytes, &g_catalog->entries, sizeof(g_catalog->entries));
  append_bytes(&state.bytes, &g_catalog->occupied, sizeof(g_catalog->occupied));
  append_bytes(&state.bytes, &g_catalog->reloadGenerations,
               sizeof(g_catalog->reloadGenerations));
  append_bytes(&state.bytes, &g_catalog->generation,
               sizeof(g_catalog->generation));
  return state;
}

/// #682: a reload whose file newly names a parent nothing has loaded, and
/// then fails on a field of its own, leaves the live state byte for byte:
/// the parent is validated, never loaded, before the child is proven good.
int verify_reload_failure_loads_no_parent(
    engine::renderer::AssetDatabase *database) {
  constexpr const char *kChildOs = "material_reload_child.json";
  constexpr const char *kChildPath = "mat/material_reload_child.json";
  constexpr const char *kParentOs = "material_reload_new_parent.json";
  constexpr const char *kParentPath = "mat/material_reload_new_parent.json";
  const auto finish = [&](int result) {
    remove_file(kChildOs);
    remove_file(kParentOs);
    return result;
  };
  const auto parentRef =
      engine::tests::catalog_material(g_catalog, kParentPath);
  if (!write_material_file(kChildOs, "{\"version\":4,\"roughness\":0.4}") ||
      !write_material_file(kParentOs, "{\"version\":4,\"metallic\":0.9}")) {
    return finish(40);
  }
  if (!engine::renderer::load_material_asset(database, g_catalog, kChildPath)
           .has_value()) {
    return finish(41);
  }

  char broken[256] = {};
  std::snprintf(broken, sizeof(broken),
                "{\"version\":4,\"parent\":\"%s\",\"roughness\":\"high\"}",
                parentRef.text);
  if (!write_material_file(kChildOs, broken)) {
    return finish(42);
  }
  const LiveStateBytes before = live_state(*database);
  const auto reloadResult =
      engine::renderer::reload_material_asset(database, g_catalog, kChildPath);
  if (reloadResult.has_value()) {
    return finish(43);
  }
  if (engine::renderer::material_asset_state(
          database, engine::content::make_asset_id_from_path(kParentPath)) !=
      engine::content::AssetState::Unloaded) {
    std::printf("the failed reload loaded the parent it named\n");
    return finish(44);
  }
  if (live_state(*database).bytes != before.bytes) {
    std::printf("the failed reload changed the live state\n");
    return finish(45);
  }
  return finish(0);
}

/// Reloading a material that was never loaded fails cleanly (no state to
/// preserve, but also no spurious registration).
int verify_reload_of_unknown_material(
    engine::renderer::AssetDatabase *database) {
  const auto reloadResult = engine::renderer::reload_material_asset(
      database, g_catalog, "mat/material_never_loaded.json");
  if (reloadResult.has_value()) {
    return 30;
  }
  return 0;
}

} // namespace

int main() {
  std::unique_ptr<engine::content::AssetCatalog> catalogOwner(
      new (std::nothrow) engine::content::AssetCatalog());
  if (catalogOwner == nullptr) {
    return 1;
  }
  g_catalog = catalogOwner.get();
  if (!engine::core::initialize_vfs()) {
    return 1;
  }
  if (!engine::core::mount("mat", ".")) {
    engine::core::shutdown_vfs();
    return 2;
  }

  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    engine::core::shutdown_vfs();
    return 3;
  }
  engine::content::clear_asset_catalog(g_catalog);

  int result = verify_reload_success(database.get());
  if (result == 0) {
    result = verify_reload_malformed_preserves_previous(database.get());
  }
  if (result == 0) {
    result = verify_reload_of_unknown_material(database.get());
  }
  if (result == 0) {
    result = verify_reload_failure_loads_no_parent(database.get());
  }

  engine::core::shutdown_vfs();
  return result;
}
