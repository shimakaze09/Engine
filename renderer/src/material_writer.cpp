// Implements JSON material asset saving for the Engine renderer system.

#include "engine/renderer/material_writer.h"

#include <cstdio>
#include <cstring>

#include "engine/content/asset_ref_json.h"
#include "engine/core/atomic_file.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/material_inheritance.h"
#include "engine/renderer/material_loader.h"

namespace engine::renderer {

namespace {

constexpr const char *kMaterialLogChannel = "material";

/// Logs a material save failure with the offending path; always false.
bool log_save_error(const char *virtualPath, const char *message) noexcept {
  char buffer[512] = {};
  std::snprintf(buffer, sizeof(buffer), "material save failed, %s: %s",
                (virtualPath != nullptr) ? virtualPath : "<null>",
                (message != nullptr) ? message : "unknown error");
  core::log_message(core::LogLevel::Error, kMaterialLogChannel, buffer);
  return false;
}

/// The shadingModel spelling the loader parses. Written for every
/// material, including the physically-based default, so a reader never
/// has to infer which model a file meant.
const char *shading_model_to_string(ShadingModel model) noexcept {
  switch (model) {
  case ShadingModel::Pbr:
    return "pbr";
  case ShadingModel::Toon:
    return "toon";
  case ShadingModel::Unlit:
    return "unlit";
  }
  return "pbr";
}

const char *alpha_mode_to_string(AlphaMode mode) noexcept {
  switch (mode) {
  case AlphaMode::Opaque:
    return "opaque";
  case AlphaMode::Mask:
    return "mask";
  case AlphaMode::Blend:
    return "blend";
  }
  return "opaque";
}

/// One overload per material field type, so the field table can write
/// every field through one name.
void write_field(core::JsonWriter *writer, const char *key,
                 const math::Vec3 &value) noexcept {
  writer->begin_array(key);
  writer->write_float_value(value.x);
  writer->write_float_value(value.y);
  writer->write_float_value(value.z);
  writer->end_array();
}
void write_field(core::JsonWriter *writer, const char *key,
                 const math::Vec2 &value) noexcept {
  writer->begin_array(key);
  writer->write_float_value(value.x);
  writer->write_float_value(value.y);
  writer->end_array();
}
void write_field(core::JsonWriter *writer, const char *key,
                 float value) noexcept {
  writer->write_float(key, value);
}
void write_field(core::JsonWriter *writer, const char *key,
                 ShadingModel value) noexcept {
  writer->write_string(key, shading_model_to_string(value));
}
void write_field(core::JsonWriter *writer, const char *key,
                 AlphaMode value) noexcept {
  writer->write_string(key, alpha_mode_to_string(value));
}

/// Writes `key` as the persistent identity of the asset behind `id`;
/// false, logged against the material, when the catalog holds no
/// identity for it. A save that wrote a path, or nothing, in its place
/// would lose the reference the next time the file moved or loaded.
bool write_catalogued_ref(core::JsonWriter *writer,
                          const content::AssetCatalog *catalog, const char *key,
                          content::AssetId id,
                          const char *virtualPath) noexcept {
  const content::AssetMetadata *metadata = find_asset_metadata(catalog, id);
  if ((metadata == nullptr) || !core::asset_ref_is_valid(metadata->ref)) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "%s names an asset with no persistent identity", key);
    return log_save_error(virtualPath, message);
  }
  content::write_asset_ref(*writer, key, metadata->ref);
  return true;
}

/// Writes one texture-slot key if its asset id is set; false when the id is
/// set but carries no identity (a save must not silently drop or corrupt a
/// texture reference).
bool write_texture_slot(core::JsonWriter *writer,
                        const content::AssetCatalog *catalog, const char *key,
                        content::AssetId textureId, bool clearsInherited,
                        const char *virtualPath) noexcept {
  if (textureId == content::kInvalidAssetId) {
    // A child that empties a slot its parent fills says so, or the next
    // load would inherit the parent's texture straight back.
    if (clearsInherited) {
      writer->write_null(key);
    }
    return true;
  }
  return write_catalogued_ref(writer, catalog, key, textureId, virtualPath);
}

} // namespace

bool find_material_parent_virtual_path(const content::AssetCatalog *catalog,
                                       content::AssetId materialId,
                                       char *outPath,
                                       std::size_t outPathCapacity) noexcept {
  if ((catalog == nullptr) || (outPath == nullptr) || (outPathCapacity == 0U)) {
    return false;
  }

  const content::AssetMetadata *parent = find_asset_metadata(
      catalog, find_material_parent_id(catalog, materialId));
  if (parent == nullptr) {
    return false;
  }
  const std::size_t pathLength = std::strlen(parent->filePath.data());
  if (pathLength >= outPathCapacity) {
    return false;
  }
  std::memcpy(outPath, parent->filePath.data(), pathLength + 1U);
  return true;
}

bool save_material_asset(const content::AssetCatalog *catalog,
                         const char *virtualPath, const Material &params,
                         const MaterialTextureSlots &textureSlots,
                         const char *parentVirtualPath,
                         std::uint16_t overriddenFields) noexcept {
  if ((catalog == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    return log_save_error(virtualPath, "invalid arguments");
  }

  core::JsonWriter writer{};
  writer.begin_object();
  writer.write_uint("version", kMaterialDocumentVersion);
  const bool hasParent =
      (parentVirtualPath != nullptr) && (parentVirtualPath[0] != '\0');
  // A material without a parent writes every field, so a reader never has
  // to infer a default; one with a parent writes only what it overrides,
  // or its parent could never reach it again.
  const std::uint16_t written =
      hasParent ? overriddenFields : material_field::kAll;
  const auto writes = [written](std::uint16_t bit) noexcept {
    return (written & bit) != 0U;
  };
  if (hasParent &&
      !write_catalogued_ref(&writer, catalog, "parent",
                            content::make_asset_id_from_path(parentVirtualPath),
                            virtualPath)) {
    return false;
  }

#define ENGINE_MATERIAL_WRITE_PARAM(name, member, key)                         \
  if (writes(material_field::k##name)) {                                       \
    write_field(&writer, key, params.member);                                  \
  }
  ENGINE_MATERIAL_PARAM_FIELDS(ENGINE_MATERIAL_WRITE_PARAM)
#undef ENGINE_MATERIAL_WRITE_PARAM

  // An inherited slot is the parent's to write; this document names only
  // the slots it overrides.
  MaterialTextureSlots ownSlots{};
  bool hasAnyTexture = false;
#define ENGINE_MATERIAL_OWN_SLOT(name, slot, handle, key)                      \
  if (writes(material_field::k##name)) {                                       \
    ownSlots.slot = textureSlots.slot;                                         \
    hasAnyTexture = hasAnyTexture || hasParent ||                              \
                    (ownSlots.slot != content::kInvalidAssetId);               \
  }
  ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_OWN_SLOT)
#undef ENGINE_MATERIAL_OWN_SLOT

  bool textureSlotsOk = true;
  if (hasAnyTexture) {
    writer.write_key("textures");
    writer.begin_object();
#define ENGINE_MATERIAL_WRITE_TEXTURE(name, slot, handle, key)                 \
  textureSlotsOk =                                                             \
      textureSlotsOk &&                                                        \
      write_texture_slot(&writer, catalog, key, ownSlots.slot,                 \
                         hasParent && writes(material_field::k##name),         \
                         virtualPath);
    ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_WRITE_TEXTURE)
#undef ENGINE_MATERIAL_WRITE_TEXTURE
    writer.end_object();
  }
  if (!textureSlotsOk) {
    return false;
  }

  writer.end_object();
  if (!writer.ok()) {
    return log_save_error(virtualPath, "JSON document build failed");
  }

  char osPath[1024] = {};
  if (!core::vfs_resolve_os_path(virtualPath, osPath, sizeof(osPath))) {
    return log_save_error(virtualPath, "virtual path is not mounted");
  }

  if (!core::atomic_write_file(osPath, writer.result(), writer.result_size())) {
    return log_save_error(virtualPath, "staged atomic write failed");
  }

  return true;
}

} // namespace engine::renderer
