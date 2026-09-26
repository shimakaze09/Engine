// Verifies save_material_asset: a saved v2 document round-trips through
// load_material_asset with identical values, an unresolvable texture-slot
// reference rejects the save without touching the destination file (staged
// atomic write internals are already covered by atomic_file_test.cpp), and
// find_material_parent_virtual_path picks the Material-tagged dependency
// out of a mix that also includes texture dependencies.

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
#include "engine/renderer/material_writer.h"

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

bool read_whole_file(const char *path, std::string *outText) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }
  char buffer[4096] = {};
  outText->clear();
  std::size_t readCount = 0U;
  while ((readCount = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    outText->append(buffer, readCount);
  }
  std::fclose(file);
  return true;
}

/// A saved material reloads with every field exactly preserved, including
/// texture-slot references (resolved back to the same AssetIds).
int verify_save_round_trip(engine::renderer::AssetDatabase *database) {
  constexpr const char *kTexturePath = "material_writer_tex_albedo.png";
  constexpr const char *kTextureVirtualPath =
      "mat/material_writer_tex_albedo.png";
  // The texture is catalogued, as the mount walk would list it, for a
  // from-scratch in-memory material an editor session would be building.
  const engine::tests::MaterialRefText textureRef =
      engine::tests::catalog_texture(g_catalog, kTextureVirtualPath);
  if (textureRef.text[0] == '\0') {
    return 10;
  }
  const engine::renderer::AssetId textureId =
      engine::renderer::make_asset_id_from_path(kTextureVirtualPath);

  engine::renderer::Material params{};
  params.albedo = engine::math::Vec3(0.2F, 0.4F, 0.6F);
  params.roughness = 0.15F;
  params.metallic = 0.9F;
  params.opacity = 0.5F;
  params.alphaMode = engine::renderer::AlphaMode::Blend;
  params.alphaCutoff = 0.42F;
  params.uvTiling = engine::math::Vec2(4.0F, 5.0F);
  params.uvOffset = engine::math::Vec2(0.1F, 0.2F);

  engine::renderer::MaterialTextureSlots slots{};
  slots.albedo = textureId;

  constexpr const char *kVirtualPath = "mat/material_writer_roundtrip.json";
  constexpr const char *kOsPath = "material_writer_roundtrip.json";
  remove_file(kOsPath);
  const bool saved = engine::renderer::save_material_asset(
      g_catalog, kVirtualPath, params, slots, nullptr,
      engine::renderer::material_field::kAll);
  if (!saved) {
    remove_file(kOsPath);
    return 11;
  }

  // The document names the texture by identity, never by where it lives.
  std::string content;
  if (!read_whole_file(kOsPath, &content) ||
      (content.find(textureRef.text) == std::string::npos) ||
      (content.find(kTextureVirtualPath) != std::string::npos)) {
    std::printf("saved document: %s\n", content.c_str());
    remove_file(kOsPath);
    return 15;
  }

  const auto loadResult =
      engine::renderer::load_material_asset(database, g_catalog, kVirtualPath);
  remove_file(kOsPath);
  remove_file(kTexturePath);
  if (!loadResult.has_value()) {
    return 12;
  }

  const engine::renderer::Material *loaded =
      engine::renderer::find_material_params(database, *loadResult);
  if ((loaded == nullptr) || !exactly_equal(loaded->albedo.x, 0.2F) ||
      !exactly_equal(loaded->albedo.y, 0.4F) ||
      !exactly_equal(loaded->albedo.z, 0.6F) ||
      !exactly_equal(loaded->roughness, 0.15F) ||
      !exactly_equal(loaded->metallic, 0.9F) ||
      !exactly_equal(loaded->opacity, 0.5F) ||
      (loaded->alphaMode != engine::renderer::AlphaMode::Blend) ||
      !exactly_equal(loaded->alphaCutoff, 0.42F) ||
      !exactly_equal(loaded->uvTiling.x, 4.0F) ||
      !exactly_equal(loaded->uvTiling.y, 5.0F) ||
      !exactly_equal(loaded->uvOffset.x, 0.1F) ||
      !exactly_equal(loaded->uvOffset.y, 0.2F)) {
    return 13;
  }

  const engine::renderer::MaterialTextureSlots *loadedSlots =
      engine::renderer::find_material_texture_slots(database, *loadResult);
  if ((loadedSlots == nullptr) || (loadedSlots->albedo != textureId)) {
    return 14;
  }

  return 0;
}

/// A texture slot whose asset has no persistent identity in the catalog
/// rejects the save and leaves a pre-existing destination file completely
/// untouched.
int verify_unresolvable_texture_rejects_save() {
  constexpr const char *kOsPath = "material_writer_unresolvable.json";
  constexpr const char *kVirtualPath = "mat/material_writer_unresolvable.json";
  constexpr const char *kOriginalContent = "{\"version\":4,\"roughness\":0.77}";
  if (!write_material_file(kOsPath, kOriginalContent)) {
    return 20;
  }

  // A texture known only by path: the record a load by file name leaves,
  // with no identity a document could name.
  engine::renderer::AssetMetadata pathOnly{};
  pathOnly.assetId = engine::renderer::make_asset_id_from_path(
      "mat/material_writer_path_only.png");
  pathOnly.typeTag = engine::renderer::AssetTypeTag::Texture;
  engine::renderer::write_metadata_path(&pathOnly.filePath,
                                        "mat/material_writer_path_only.png");
  if (!engine::content::register_asset_metadata(g_catalog, pathOnly)) {
    remove_file(kOsPath);
    return 24;
  }

  // An id with no registered metadata at all, then one with no identity.
  const engine::renderer::AssetId kUnsavable[] = {0xDEADBEEFULL,
                                                  pathOnly.assetId};
  for (const engine::renderer::AssetId id : kUnsavable) {
    engine::renderer::Material params{};
    engine::renderer::MaterialTextureSlots slots{};
    slots.albedo = id;
    const bool saved = engine::renderer::save_material_asset(
        g_catalog, kVirtualPath, params, slots, nullptr,
        engine::renderer::material_field::kAll);
    if (saved) {
      remove_file(kOsPath);
      return 21;
    }
  }

  std::string afterContent;
  if (!read_whole_file(kOsPath, &afterContent)) {
    remove_file(kOsPath);
    return 22;
  }
  remove_file(kOsPath);
  if (afterContent != kOriginalContent) {
    return 23;
  }

  return 0;
}

/// find_material_parent_virtual_path picks the Material-tagged dependency
/// out of a mix that also has a texture dependency, and reports false when
/// there is no parent at all.
int verify_find_parent_path(engine::renderer::AssetDatabase *database) {
  constexpr const char *kParentPath = "material_writer_parent.json";
  constexpr const char *kParentVirtualPath = "mat/material_writer_parent.json";
  constexpr const char *kChildPath = "material_writer_child.json";
  constexpr const char *kChildVirtualPath = "mat/material_writer_child.json";

  char childJson[256] = {};
  std::snprintf(
      childJson, sizeof(childJson),
      "{\"version\":4,\"parent\":\"%s\",\"textures\":{\"albedo\":\"%s\"}}",
      engine::tests::catalog_material(g_catalog, kParentVirtualPath).text,
      engine::tests::catalog_texture(g_catalog, "assets/textures/child.png")
          .text);
  if (!write_material_file(kParentPath, "{\"version\":4,\"roughness\":0.5}") ||
      !write_material_file(kChildPath, childJson)) {
    remove_file(kParentPath);
    remove_file(kChildPath);
    return 30;
  }

  const auto childResult = engine::renderer::load_material_asset(
      database, g_catalog, kChildVirtualPath);
  remove_file(kParentPath);
  remove_file(kChildPath);
  if (!childResult.has_value()) {
    return 31;
  }

  char parentPath[260] = {};
  if (!engine::renderer::find_material_parent_virtual_path(
          g_catalog, *childResult, parentPath, sizeof(parentPath))) {
    return 32;
  }
  if (std::strcmp(parentPath, kParentVirtualPath) != 0) {
    return 33;
  }

  // The parent itself has no parent.
  const engine::renderer::AssetId parentId =
      engine::renderer::make_asset_id_from_path(kParentVirtualPath);
  char noParentPath[260] = {};
  if (engine::renderer::find_material_parent_virtual_path(
          g_catalog, parentId, noParentPath, sizeof(noParentPath))) {
    return 34;
  }

  return 0;
}

/// A material with a parent writes only its overrides (#543): the parent
/// key, the fields named in the mask, and no inherited value or slot -- not
/// even one whose path could not be written.
int verify_child_writes_only_overrides() {
  constexpr const char *kOsPath = "material_writer_child.json";
  constexpr const char *kVirtualPath = "mat/material_writer_child.json";
  remove_file(kOsPath);

  engine::renderer::Material params{};
  params.roughness = 0.25F;
  params.metallic = 0.75F;
  engine::renderer::MaterialTextureSlots slots{};
  slots.albedo = 0xDEADBEEFULL; // inherited, and unresolvable

  // A parent the catalog holds no identity for cannot be named, so the
  // save is refused rather than writing a path or dropping the parent.
  if (engine::renderer::save_material_asset(
          g_catalog, kVirtualPath, params, slots, "mat/material_parent.json",
          engine::renderer::material_field::kRoughness)) {
    remove_file(kOsPath);
    return 43;
  }
  const engine::tests::MaterialRefText parentRef =
      engine::tests::catalog_material(g_catalog, "mat/material_parent.json");
  if (!engine::renderer::save_material_asset(
          g_catalog, kVirtualPath, params, slots, "mat/material_parent.json",
          engine::renderer::material_field::kRoughness)) {
    remove_file(kOsPath);
    return 40;
  }
  std::string content;
  const bool read = read_whole_file(kOsPath, &content);
  remove_file(kOsPath);
  if (!read) {
    return 41;
  }
  const std::string parentKey =
      std::string("\"parent\":\"") + parentRef.text + "\"";
  if ((content.find(parentKey) == std::string::npos) ||
      (content.find("\"roughness\"") == std::string::npos) ||
      (content.find("\"metallic\"") != std::string::npos) ||
      (content.find("\"albedo\"") != std::string::npos) ||
      (content.find("\"shadingModel\"") != std::string::npos) ||
      (content.find("\"textures\"") != std::string::npos)) {
    std::printf("child document: %s\n", content.c_str());
    return 42;
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

  int result = verify_save_round_trip(database.get());
  if (result == 0) {
    result = verify_unresolvable_texture_rejects_save();
  }
  if (result == 0) {
    result = verify_find_parent_path(database.get());
  }
  if (result == 0) {
    result = verify_child_writes_only_overrides();
  }

  engine::core::shutdown_vfs();
  return result;
}
