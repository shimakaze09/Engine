// Implements JSON material asset saving for the Engine renderer system.

#include "engine/renderer/material_writer.h"

#include <cstdio>
#include <cstring>

#include "engine/core/atomic_file.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/material_inheritance.h"

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

/// Writes one texture-slot key if its asset id is set; false when the id is
/// set but its source path cannot be resolved (a save must not silently
/// drop or corrupt a texture reference).
bool write_texture_slot(core::JsonWriter *writer, const AssetDatabase *database,
                        const char *key, AssetId textureId) noexcept {
  if (textureId == kInvalidAssetId) {
    return true;
  }

  const AssetMetadata *metadata = find_asset_metadata(database, textureId);
  if ((metadata == nullptr) || (metadata->filePath[0] == '\0')) {
    return false;
  }

  writer->write_string(key, metadata->filePath.data());
  return true;
}

} // namespace

bool find_material_parent_virtual_path(const AssetDatabase *database,
                                       AssetId materialId, char *outPath,
                                       std::size_t outPathCapacity) noexcept {
  if ((database == nullptr) || (outPath == nullptr) ||
      (outPathCapacity == 0U)) {
    return false;
  }

  const AssetMetadata *parent = find_asset_metadata(
      database, find_material_parent_id(database, materialId));
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

bool save_material_asset(const AssetDatabase *database, const char *virtualPath,
                         const Material &params,
                         const MaterialTextureSlots &textureSlots,
                         const char *parentVirtualPath,
                         std::uint16_t overriddenFields) noexcept {
  if ((database == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    return log_save_error(virtualPath, "invalid arguments");
  }

  core::JsonWriter writer{};
  writer.begin_object();
  writer.write_uint("version", 3U);
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
  if (hasParent) {
    writer.write_string("parent", parentVirtualPath);
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
    hasAnyTexture = hasAnyTexture || (ownSlots.slot != kInvalidAssetId);       \
  }
  ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_OWN_SLOT)
#undef ENGINE_MATERIAL_OWN_SLOT

  bool textureSlotsOk = true;
  if (hasAnyTexture) {
    writer.write_key("textures");
    writer.begin_object();
#define ENGINE_MATERIAL_WRITE_TEXTURE(name, slot, handle, key)                 \
  textureSlotsOk = textureSlotsOk &&                                           \
                   write_texture_slot(&writer, database, key, ownSlots.slot);
    ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_WRITE_TEXTURE)
#undef ENGINE_MATERIAL_WRITE_TEXTURE
    writer.end_object();
  }
  if (!textureSlotsOk) {
    return log_save_error(virtualPath,
                          "a texture slot's source path is unresolvable");
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
