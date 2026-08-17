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

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"
#include "engine/renderer/material_writer.h"

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
  // A texture slot's source path must already be registered as metadata
  // (normally the loader does this); simulate that here for a from-scratch
  // in-memory material a material editor session would be building.
  const engine::renderer::AssetId textureId =
      engine::renderer::make_asset_id_from_path(kTextureVirtualPath);
  engine::renderer::AssetMetadata textureMetadata{};
  textureMetadata.assetId = textureId;
  textureMetadata.typeTag = engine::renderer::AssetTypeTag::Texture;
  engine::renderer::write_metadata_path(&textureMetadata.filePath,
                                        kTextureVirtualPath);
  if (!engine::renderer::register_asset_metadata(database, textureMetadata)) {
    return 10;
  }

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
      database, kVirtualPath, params, slots, nullptr);
  if (!saved) {
    remove_file(kOsPath);
    return 11;
  }

  const auto loadResult =
      engine::renderer::load_material_asset(database, kVirtualPath);
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

/// A texture slot id with no resolvable metadata path rejects the save and
/// leaves a pre-existing destination file completely untouched.
int verify_unresolvable_texture_rejects_save(
    engine::renderer::AssetDatabase *database) {
  constexpr const char *kOsPath = "material_writer_unresolvable.json";
  constexpr const char *kVirtualPath = "mat/material_writer_unresolvable.json";
  constexpr const char *kOriginalContent = "{\"version\":1,\"roughness\":0.77}";
  if (!write_material_file(kOsPath, kOriginalContent)) {
    return 20;
  }

  engine::renderer::Material params{};
  engine::renderer::MaterialTextureSlots slots{};
  // An id with no registered metadata at all.
  slots.albedo = 0xDEADBEEFULL;

  const bool saved = engine::renderer::save_material_asset(
      database, kVirtualPath, params, slots, nullptr);
  if (saved) {
    remove_file(kOsPath);
    return 21;
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

  if (!write_material_file(kParentPath, "{\"version\":2,\"roughness\":0.5}") ||
      !write_material_file(
          kChildPath,
          "{\"version\":2,\"parent\":\"mat/material_writer_parent.json\","
          "\"textures\":{\"albedo\":\"assets/textures/child.png\"}}")) {
    remove_file(kParentPath);
    remove_file(kChildPath);
    return 30;
  }

  const auto childResult =
      engine::renderer::load_material_asset(database, kChildVirtualPath);
  remove_file(kParentPath);
  remove_file(kChildPath);
  if (!childResult.has_value()) {
    return 31;
  }

  char parentPath[260] = {};
  if (!engine::renderer::find_material_parent_virtual_path(
          database, *childResult, parentPath, sizeof(parentPath))) {
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
          database, parentId, noParentPath, sizeof(noParentPath))) {
    return 34;
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

  int result = verify_save_round_trip(database.get());
  if (result == 0) {
    result = verify_unresolvable_texture_rejects_save(database.get());
  }
  if (result == 0) {
    result = verify_find_parent_path(database.get());
  }

  engine::core::shutdown_vfs();
  return result;
}
