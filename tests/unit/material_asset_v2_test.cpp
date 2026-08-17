// Verifies the v2 texture-backed material schema: full field round trip,
// texture-slot dependency edges, parent-chain texture override semantics,
// and the version gate that keeps v1 files from picking up v2-only fields.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"

namespace {

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

/// A full v2 file: every new field present must land exactly, and every
/// texture slot must register Texture metadata plus a dependency edge.
int verify_v2_full_load(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_v2_full.json";
  constexpr const char *kVirtualPath = "mat/material_v2_full.json";
  constexpr const char *kJson =
      "{\"version\":2,\"albedo\":[0.5,0.5,0.5],\"roughness\":0.4,"
      "\"alphaMode\":\"mask\",\"alphaCutoff\":0.3,"
      "\"uvTiling\":[2.0,3.0],\"uvOffset\":[0.25,0.75],"
      "\"textures\":{"
      "\"albedo\":\"assets/textures/albedo.png\","
      "\"metallicRoughness\":\"assets/textures/mr.png\","
      "\"emissive\":\"assets/textures/emissive.png\","
      "\"occlusion\":\"assets/textures/ao.png\","
      "\"opacity\":\"assets/textures/opacity.png\"}}";
  if (!write_material_file(kPath, kJson)) {
    return 10;
  }

  const auto loadResult =
      engine::renderer::load_material_asset(database, kVirtualPath);
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

  // Every texture slot registered Texture-tagged metadata...
  const engine::renderer::AssetMetadata *albedoMeta =
      engine::renderer::find_asset_metadata(database, expectedAlbedo);
  if ((albedoMeta == nullptr) ||
      (albedoMeta->typeTag != engine::renderer::AssetTypeTag::Texture)) {
    return 17;
  }
  // ...and a dependency edge from the material to each texture.
  engine::renderer::AssetId deps[8] = {};
  const std::size_t depCount =
      engine::renderer::get_dependencies(database, id, deps, 8U);
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

/// Malformed v2-only fields (bad alphaMode string, non-object "textures",
/// empty texture path) reject the load; the version gate itself (out of
/// range) is covered by material_asset_test.cpp's failure suite.
int verify_v2_malformed_fields(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_v2_bad.json";
  constexpr const char *kVirtualPath = "mat/material_v2_bad.json";

  if (!write_material_file(kPath, "{\"version\":2,\"alphaMode\":\"glow\"}")) {
    return 30;
  }
  auto result = engine::renderer::load_material_asset(database, kVirtualPath);
  remove_file(kPath);
  if (result.has_value()) {
    return 31;
  }

  if (!write_material_file(kPath, "{\"version\":2,\"textures\":[1,2]}")) {
    return 32;
  }
  result = engine::renderer::load_material_asset(database, kVirtualPath);
  remove_file(kPath);
  if (result.has_value()) {
    return 33;
  }

  if (!write_material_file(kPath,
                           "{\"version\":2,\"textures\":{\"albedo\":\"\"}}")) {
    return 34;
  }
  result = engine::renderer::load_material_asset(database, kVirtualPath);
  remove_file(kPath);
  if (result.has_value()) {
    return 35;
  }

  return 0;
}

/// A v1-declared file that happens to carry v2-only keys ignores them
/// entirely (the version gate, not merely "fields default when absent").
int verify_v1_ignores_v2_fields(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_v1_with_v2_keys.json";
  constexpr const char *kVirtualPath = "mat/material_v1_with_v2_keys.json";
  constexpr const char *kJson =
      "{\"version\":1,\"alphaMode\":\"mask\",\"alphaCutoff\":0.9,"
      "\"uvTiling\":[9.0,9.0],"
      "\"textures\":{\"albedo\":\"assets/textures/should_be_ignored.png\"}}";
  if (!write_material_file(kPath, kJson)) {
    return 40;
  }

  const auto loadResult =
      engine::renderer::load_material_asset(database, kVirtualPath);
  remove_file(kPath);
  if (!loadResult.has_value()) {
    return 41;
  }
  const engine::renderer::AssetId id = *loadResult;

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  const engine::renderer::Material defaults{};
  if ((params == nullptr) || (params->alphaMode != defaults.alphaMode) ||
      !exactly_equal(params->alphaCutoff, defaults.alphaCutoff) ||
      !exactly_equal(params->uvTiling.x, defaults.uvTiling.x)) {
    return 42;
  }

  const engine::renderer::MaterialTextureSlots *slots =
      engine::renderer::find_material_texture_slots(database, id);
  if ((slots == nullptr) ||
      (slots->albedo != engine::renderer::kInvalidAssetId)) {
    return 43;
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

  if (!write_material_file(
          kBasePath,
          "{\"version\":2,\"textures\":{"
          "\"albedo\":\"assets/textures/base_albedo.png\","
          "\"emissive\":\"assets/textures/base_emissive.png\"}}") ||
      !write_material_file(
          kChildPath,
          "{\"version\":2,\"parent\":\"mat/material_v2_tex_base.json\","
          "\"textures\":{\"albedo\":\"assets/textures/child_albedo.png\"}}")) {
    remove_file(kBasePath);
    remove_file(kChildPath);
    return 50;
  }

  const auto childResult = engine::renderer::load_material_asset(
      database, "mat/material_v2_tex_child.json");
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

} // namespace

int main() {
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

  int result = verify_v2_full_load(database.get());
  if (result == 0) {
    result = verify_v2_malformed_fields(database.get());
  }
  if (result == 0) {
    result = verify_v1_ignores_v2_fields(database.get());
  }
  if (result == 0) {
    result = verify_v2_parent_texture_override(database.get());
  }

  engine::core::shutdown_vfs();
  return result;
}
