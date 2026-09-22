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

  if (writes(material_field::kAlbedo)) {
    writer.begin_array("albedo");
    writer.write_float_value(params.albedo.x);
    writer.write_float_value(params.albedo.y);
    writer.write_float_value(params.albedo.z);
    writer.end_array();
  }

  if (writes(material_field::kEmissive)) {
    writer.begin_array("emissive");
    writer.write_float_value(params.emissive.x);
    writer.write_float_value(params.emissive.y);
    writer.write_float_value(params.emissive.z);
    writer.end_array();
  }

  if (writes(material_field::kRoughness)) {
    writer.write_float("roughness", params.roughness);
  }
  if (writes(material_field::kMetallic)) {
    writer.write_float("metallic", params.metallic);
  }
  if (writes(material_field::kOpacity)) {
    writer.write_float("opacity", params.opacity);
  }
  if (writes(material_field::kShadingModel)) {
    writer.write_string("shadingModel",
                        shading_model_to_string(params.shadingModel));
  }
  if (writes(material_field::kAlphaMode)) {
    writer.write_string("alphaMode", alpha_mode_to_string(params.alphaMode));
  }
  if (writes(material_field::kAlphaCutoff)) {
    writer.write_float("alphaCutoff", params.alphaCutoff);
  }

  if (writes(material_field::kUvTiling)) {
    writer.begin_array("uvTiling");
    writer.write_float_value(params.uvTiling.x);
    writer.write_float_value(params.uvTiling.y);
    writer.end_array();
  }

  if (writes(material_field::kUvOffset)) {
    writer.begin_array("uvOffset");
    writer.write_float_value(params.uvOffset.x);
    writer.write_float_value(params.uvOffset.y);
    writer.end_array();
  }

  // An inherited slot is the parent's to write; this document names only
  // the slots it overrides.
  MaterialTextureSlots ownSlots{};
  ownSlots.albedo = writes(material_field::kAlbedoTexture) ? textureSlots.albedo
                                                           : kInvalidAssetId;
  ownSlots.metallicRoughness = writes(material_field::kMetallicRoughnessTexture)
                                   ? textureSlots.metallicRoughness
                                   : kInvalidAssetId;
  ownSlots.emissive = writes(material_field::kEmissiveTexture)
                          ? textureSlots.emissive
                          : kInvalidAssetId;
  ownSlots.occlusion = writes(material_field::kOcclusionTexture)
                           ? textureSlots.occlusion
                           : kInvalidAssetId;
  ownSlots.opacity = writes(material_field::kOpacityTexture)
                         ? textureSlots.opacity
                         : kInvalidAssetId;

  const bool hasAnyTexture = (ownSlots.albedo != kInvalidAssetId) ||
                             (ownSlots.metallicRoughness != kInvalidAssetId) ||
                             (ownSlots.emissive != kInvalidAssetId) ||
                             (ownSlots.occlusion != kInvalidAssetId) ||
                             (ownSlots.opacity != kInvalidAssetId);
  bool textureSlotsOk = true;
  if (hasAnyTexture) {
    writer.write_key("textures");
    writer.begin_object();
    textureSlotsOk =
        write_texture_slot(&writer, database, "albedo", ownSlots.albedo) &&
        write_texture_slot(&writer, database, "metallicRoughness",
                           ownSlots.metallicRoughness) &&
        write_texture_slot(&writer, database, "emissive", ownSlots.emissive) &&
        write_texture_slot(&writer, database, "occlusion",
                           ownSlots.occlusion) &&
        write_texture_slot(&writer, database, "opacity", ownSlots.opacity);
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
