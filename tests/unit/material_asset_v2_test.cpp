// Verifies the texture-backed material schema: full field round trip,
// texture-slot dependency edges, parent-chain texture override semantics,
// references that follow a moved parent and texture,
// and that every field the format defines is read at the one revision the
// build accepts -- there is no revision at which a present field is
// silently ignored.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

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

/// A full file: every field present must land exactly, and every texture
/// slot must resolve to its catalogued texture plus a dependency edge.
int verify_v2_full_load(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_v2_full.json";
  constexpr const char *kVirtualPath = "mat/material_v2_full.json";
  char kJson[640] = {};
  std::snprintf(
      kJson, sizeof(kJson),
      "{\"version\":4,\"albedo\":[0.5,0.5,0.5],\"roughness\":0.4,"
      "\"alphaMode\":\"mask\",\"alphaCutoff\":0.3,"
      "\"uvTiling\":[2.0,3.0],\"uvOffset\":[0.25,0.75],"
      "\"textures\":{\"albedo\":\"%s\",\"metallicRoughness\":\"%s\","
      "\"emissive\":\"%s\",\"occlusion\":\"%s\",\"opacity\":\"%s\"}}",
      engine::tests::catalog_texture(g_catalog, "assets/textures/albedo.png")
          .text,
      engine::tests::catalog_texture(g_catalog, "assets/textures/mr.png").text,
      engine::tests::catalog_texture(g_catalog, "assets/textures/emissive.png")
          .text,
      engine::tests::catalog_texture(g_catalog, "assets/textures/ao.png").text,
      engine::tests::catalog_texture(g_catalog, "assets/textures/opacity.png")
          .text);
  if (!write_material_file(kPath, kJson)) {
    return 10;
  }

  const auto loadResult =
      engine::renderer::load_material_asset(database, g_catalog, kVirtualPath);
  remove_file(kPath);
  if (!loadResult.has_value()) {
    return 11;
  }
  const engine::renderer::AssetId id = *loadResult;

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  if (params == nullptr) {
    return 12;
  }
  if ((params->alphaMode != engine::renderer::AlphaMode::Mask) ||
      !exactly_equal(params->alphaCutoff, 0.3F) ||
      !exactly_equal(params->uvTiling.x, 2.0F) ||
      !exactly_equal(params->uvTiling.y, 3.0F) ||
      !exactly_equal(params->uvOffset.x, 0.25F) ||
      !exactly_equal(params->uvOffset.y, 0.75F)) {
    return 13;
  }
  // Texture handles are only populated by resolve_material_textures (needs
  // a GL context); a bare load leaves every slot unresolved.
  if ((params->albedoTexture != engine::renderer::kInvalidTextureHandle) ||
      (params->metallicRoughnessTexture !=
       engine::renderer::kInvalidTextureHandle) ||
      (params->emissiveTexture != engine::renderer::kInvalidTextureHandle) ||
      (params->occlusionTexture != engine::renderer::kInvalidTextureHandle) ||
      (params->opacityTexture != engine::renderer::kInvalidTextureHandle)) {
    return 14;
  }

  const engine::renderer::MaterialTextureSlots *slots =
      engine::renderer::find_material_texture_slots(database, id);
  if (slots == nullptr) {
    return 15;
  }
  const engine::renderer::AssetId expectedAlbedo =
      engine::renderer::make_asset_id_from_path("assets/textures/albedo.png");
  const engine::renderer::AssetId expectedMr =
      engine::renderer::make_asset_id_from_path("assets/textures/mr.png");
  const engine::renderer::AssetId expectedEmissive =
      engine::renderer::make_asset_id_from_path(
          "assets/textures/emissive.png");
  const engine::renderer::AssetId expectedAo =
      engine::renderer::make_asset_id_from_path("assets/textures/ao.png");
  const engine::renderer::AssetId expectedOpacity =
      engine::renderer::make_asset_id_from_path(
          "assets/textures/opacity.png");
  if ((slots->albedo != expectedAlbedo) ||
      (slots->metallicRoughness != expectedMr) ||
      (slots->emissive != expectedEmissive) ||
      (slots->occlusion != expectedAo) || (slots->opacity != expectedOpacity)) {
    return 16;
  }

  // Every texture slot resolved to the catalogued Texture record...
  const engine::renderer::AssetMetadata *albedoMeta =
      engine::content::find_asset_metadata(g_catalog, expectedAlbedo);
  if ((albedoMeta == nullptr) ||
      (albedoMeta->typeTag != engine::renderer::AssetTypeTag::Texture)) {
    return 17;
  }
  // ...and a dependency edge from the material to each texture.
  engine::renderer::AssetId deps[8] = {};
  const std::size_t depCount =
      engine::content::get_dependencies(g_catalog, id, deps, 8U);
  if (depCount != 5U) {
    return 18;
  }
  bool sawAlbedoDep = false;
  for (std::size_t i = 0U; i < depCount; ++i) {
    if (deps[i] == expectedAlbedo) {
      sawAlbedoDep = true;
    }
  }
  if (!sawAlbedoDep) {
    return 19;
  }

  return 0;
}

/// Malformed fields (bad alphaMode string, non-object "textures", a
/// texture slot that names no catalogued texture) reject the load; the version
/// gate itself (out of range) is covered by material_asset_test.cpp's failure
/// suite.
int verify_v2_malformed_fields(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_v2_bad.json";
  constexpr const char *kVirtualPath = "mat/material_v2_bad.json";

  if (!write_material_file(kPath, "{\"version\":4,\"alphaMode\":\"glow\"}")) {
    return 30;
  }
  auto result =
      engine::renderer::load_material_asset(database, g_catalog, kVirtualPath);
  remove_file(kPath);
  if (result.has_value()) {
    return 31;
  }

  if (!write_material_file(kPath, "{\"version\":4,\"textures\":[1,2]}")) {
    return 32;
  }
  result =
      engine::renderer::load_material_asset(database, g_catalog, kVirtualPath);
  remove_file(kPath);
  if (result.has_value()) {
    return 33;
  }

  // Not a reference at all: empty text, and the path spelling of the
  // previous revision, which names nothing now. Then a reference the
  // catalog has no asset for, and one naming an asset that is not a
  // texture. Each refuses the load rather than dropping the slot, so a
  // later save cannot write the material back without it.
  const engine::tests::MaterialRefText materialRef =
      engine::tests::catalog_material(g_catalog, "mat/material_v2_other.json");
  char uncatalogued[engine::content::kAssetRefTextLength + 1U] = {};
  static_cast<void>(engine::content::format_asset_ref(
      engine::core::asset_ref_primary(
          engine::content::builtin_asset_guid("assets/textures/gone.png")),
      uncatalogued, sizeof(uncatalogued)));
  const char *const kRefusedSlots[] = {"", "assets/textures/albedo.png",
                                       uncatalogued, materialRef.text};
  for (const char *slot : kRefusedSlots) {
    char json[160] = {};
    std::snprintf(json, sizeof(json),
                  "{\"version\":4,\"textures\":{\"albedo\":\"%s\"}}", slot);
    if (!write_material_file(kPath, json)) {
      return 34;
    }
    result = engine::renderer::load_material_asset(database, g_catalog,
                                                   kVirtualPath);
    remove_file(kPath);
    if (result.has_value()) {
      return 35;
    }
  }

  return 0;
}

/// Every field the format defines is read, and an authored value is never
/// dropped in favor of the default. This replaces the staged-migration
/// contract, where a file declaring the older revision ignored the newer
/// revision's keys: that layer is gone, so a present field either takes
/// effect or refuses the document, and nothing in between.
int verify_every_field_is_read(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_all_fields.json";
  constexpr const char *kVirtualPath = "mat/material_all_fields.json";
  constexpr const char *kJson = "{\"version\":4,\"shadingModel\":\"toon\","
                                "\"alphaMode\":\"mask\",\"alphaCutoff\":0.9,"
                                "\"uvTiling\":[9.0,9.0]}";
  if (!write_material_file(kPath, kJson)) {
    return 40;
  }

  const auto loadResult =
      engine::renderer::load_material_asset(database, g_catalog, kVirtualPath);
  remove_file(kPath);
  if (!loadResult.has_value()) {
    return 41;
  }
  const engine::renderer::AssetId id = *loadResult;

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  const engine::renderer::Material defaults{};
  if ((params == nullptr) ||
      (params->shadingModel != engine::renderer::ShadingModel::Toon) ||
      (params->alphaMode != engine::renderer::AlphaMode::Mask) ||
      !exactly_equal(params->alphaCutoff, 0.9F) ||
      !exactly_equal(params->uvTiling.x, 9.0F) ||
      !exactly_equal(params->uvTiling.y, 9.0F)) {
    return 42;
  }
  // A field the document omits still takes the default, so "every field is
  // read" is not "every field is required".
  if (!exactly_equal(params->uvOffset.x, defaults.uvOffset.x)) {
    return 43;
  }

  // A shading model this build does not know refuses the document rather
  // than lighting the surface by a model the author did not ask for.
  constexpr const char *kUnknownPath = "material_unknown_model.json";
  constexpr const char *kUnknownVirtualPath =
      "mat/material_unknown_model.json";
  constexpr const char *kUnknownModel =
      "{\"version\":4,\"shadingModel\":\"cel\"}";
  if (!write_material_file(kUnknownPath, kUnknownModel)) {
    return 44;
  }
  const auto unknownResult = engine::renderer::load_material_asset(
      database, g_catalog, kUnknownVirtualPath);
  remove_file(kUnknownPath);
  if (unknownResult.has_value()) {
    return 45;
  }

  return 0;
}

/// A v2 child overriding one texture slot does not disturb slots it did not
/// mention (inherited from the v2 parent), matching the existing
/// scalar/vector parent-chain contract.
int verify_v2_parent_texture_override(
    engine::renderer::AssetDatabase *database) {
  constexpr const char *kBasePath = "material_v2_tex_base.json";
  constexpr const char *kChildPath = "material_v2_tex_child.json";

  char baseJson[256] = {};
  char childJson[256] = {};
  std::snprintf(
      baseJson, sizeof(baseJson),
      "{\"version\":4,\"textures\":{\"albedo\":\"%s\",\"emissive\":\"%s\"}}",
      engine::tests::catalog_texture(g_catalog,
                                     "assets/textures/base_albedo.png")
          .text,
      engine::tests::catalog_texture(g_catalog,
                                     "assets/textures/base_emissive.png")
          .text);
  std::snprintf(
      childJson, sizeof(childJson),
      "{\"version\":4,\"parent\":\"%s\",\"textures\":{\"albedo\":\"%s\"}}",
      engine::tests::catalog_material(g_catalog,
                                      "mat/material_v2_tex_base.json")
          .text,
      engine::tests::catalog_texture(g_catalog,
                                     "assets/textures/child_albedo.png")
          .text);
  if (!write_material_file(kBasePath, baseJson) ||
      !write_material_file(kChildPath, childJson)) {
    remove_file(kBasePath);
    remove_file(kChildPath);
    return 50;
  }

  const auto childResult = engine::renderer::load_material_asset(
      database, g_catalog, "mat/material_v2_tex_child.json");
  remove_file(kBasePath);
  remove_file(kChildPath);
  if (!childResult.has_value()) {
    return 51;
  }

  const engine::renderer::MaterialTextureSlots *slots =
      engine::renderer::find_material_texture_slots(database, *childResult);
  if (slots == nullptr) {
    return 52;
  }
  const engine::renderer::AssetId expectedChildAlbedo =
      engine::renderer::make_asset_id_from_path(
          "assets/textures/child_albedo.png");
  const engine::renderer::AssetId expectedBaseEmissive =
      engine::renderer::make_asset_id_from_path(
          "assets/textures/base_emissive.png");
  // Overridden slot wins...
  if (slots->albedo != expectedChildAlbedo) {
    return 53;
  }
  // ...untouched slot inherits the parent's reference.
  if (slots->emissive != expectedBaseEmissive) {
    return 54;
  }
  // The child never named a metallicRoughness/occlusion/opacity texture at
  // any level of the chain, so those stay unset.
  if ((slots->metallicRoughness != engine::renderer::kInvalidAssetId) ||
      (slots->occlusion != engine::renderer::kInvalidAssetId) ||
      (slots->opacity != engine::renderer::kInvalidAssetId)) {
    return 55;
  }

  return 0;
}

/// A document names its parent and textures by identity, so the same
/// bytes still resolve after both are moved: the catalog of a later
/// session lists them at new paths under the GUIDs their sidecars kept,
/// and the child follows them there. A path-naming document would point
/// at nothing after the move.
int verify_moved_assets_still_resolve() {
  constexpr const char *kChildPath = "material_moved_child.json";
  constexpr const char *kChildVirtualPath = "mat/material_moved_child.json";
  constexpr const char *kParentPaths[] = {"material_moved_parent_old.json",
                                          "material_moved_parent_new.json"};
  constexpr const char *kParentVirtualPaths[] = {
      "mat/material_moved_parent_old.json",
      "mat/material_moved_parent_new.json"};
  constexpr const char *kTextureVirtualPaths[] = {
      "assets/textures/before/albedo.png", "assets/textures/after/albedo.png"};

  std::string childJson;
  for (std::size_t session = 0U; session < 2U; ++session) {
    std::unique_ptr<engine::renderer::AssetDatabase> database(
        new (std::nothrow) engine::renderer::AssetDatabase());
    if (database == nullptr) {
      return 60;
    }
    engine::content::clear_asset_catalog(g_catalog);
    const engine::tests::MaterialRefText parentRef =
        engine::tests::catalog_material_asset(
            g_catalog, kParentVirtualPaths[session],
            engine::renderer::AssetTypeTag::Material, "moved-parent");
    const engine::tests::MaterialRefText textureRef =
        engine::tests::catalog_material_asset(
            g_catalog, kTextureVirtualPaths[session],
            engine::renderer::AssetTypeTag::Texture, "moved-albedo");
    if (session == 0U) {
      char json[256] = {};
      std::snprintf(json, sizeof(json),
                    "{\"version\":4,\"parent\":\"%s\","
                    "\"textures\":{\"albedo\":\"%s\"}}",
                    parentRef.text, textureRef.text);
      childJson = json;
      if (!write_material_file(kChildPath, childJson.c_str())) {
        return 61;
      }
    }
    // Only the current session's parent file exists: the move took the
    // old one away.
    if (!write_material_file(kParentPaths[session],
                             "{\"version\":4,\"roughness\":0.625}")) {
      remove_file(kChildPath);
      return 62;
    }

    const auto childResult = engine::renderer::load_material_asset(
        database.get(), g_catalog, kChildVirtualPath);
    remove_file(kParentPaths[session]);
    if (!childResult.has_value()) {
      remove_file(kChildPath);
      return 63 + static_cast<int>(session);
    }
    const engine::renderer::Material *params =
        engine::renderer::find_material_params(database.get(), *childResult);
    const engine::renderer::MaterialTextureSlots *slots =
        engine::renderer::find_material_texture_slots(database.get(),
                                                      *childResult);
    if ((params == nullptr) || !exactly_equal(params->roughness, 0.625F) ||
        (slots == nullptr) ||
        (slots->albedo != engine::renderer::make_asset_id_from_path(
                              kTextureVirtualPaths[session]))) {
      remove_file(kChildPath);
      return 65 + static_cast<int>(session);
    }
  }
  remove_file(kChildPath);
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

  int result = verify_v2_full_load(database.get());
  if (result == 0) {
    result = verify_v2_malformed_fields(database.get());
  }
  if (result == 0) {
    result = verify_every_field_is_read(database.get());
  }
  if (result == 0) {
    result = verify_v2_parent_texture_override(database.get());
  }
  if (result == 0) {
    result = verify_moved_assets_still_resolve();
  }

  engine::core::shutdown_vfs();
  return result;
}
