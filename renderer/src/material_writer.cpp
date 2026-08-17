// Implements v2 JSON material asset saving for the Engine renderer system.

#include "engine/renderer/material_writer.h"

#include <cstdio>
#include <cstring>

#include "engine/core/atomic_file.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"

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

  constexpr std::size_t kMaxDeps = AssetMetadata::kMaxDependencies;
  AssetId deps[kMaxDeps] = {};
  const std::size_t depCount =
      get_dependencies(database, materialId, deps, kMaxDeps);
  for (std::size_t i = 0U; i < depCount; ++i) {
    const AssetMetadata *depMetadata = find_asset_metadata(database, deps[i]);
    if ((depMetadata != nullptr) &&
        (depMetadata->typeTag == AssetTypeTag::Material)) {
      const std::size_t pathLength = std::strlen(depMetadata->filePath.data());
      if (pathLength >= outPathCapacity) {
        return false;
      }
      std::memcpy(outPath, depMetadata->filePath.data(), pathLength + 1U);
      return true;
    }
  }

  return false;
}

bool save_material_asset(const AssetDatabase *database,
                         const char *virtualPath, const Material &params,
                         const MaterialTextureSlots &textureSlots,
                         const char *parentVirtualPath) noexcept {
  if ((database == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    return log_save_error(virtualPath, "invalid arguments");
  }

  core::JsonWriter writer{};
  writer.begin_object();
  writer.write_uint("version", 2U);
  if ((parentVirtualPath != nullptr) && (parentVirtualPath[0] != '\0')) {
    writer.write_string("parent", parentVirtualPath);
  }

  writer.begin_array("albedo");
  writer.write_float_value(params.albedo.x);
  writer.write_float_value(params.albedo.y);
  writer.write_float_value(params.albedo.z);
  writer.end_array();

  writer.begin_array("emissive");
  writer.write_float_value(params.emissive.x);
  writer.write_float_value(params.emissive.y);
  writer.write_float_value(params.emissive.z);
  writer.end_array();

  writer.write_float("roughness", params.roughness);
  writer.write_float("metallic", params.metallic);
  writer.write_float("opacity", params.opacity);
  writer.write_string("alphaMode", alpha_mode_to_string(params.alphaMode));
  writer.write_float("alphaCutoff", params.alphaCutoff);

  writer.begin_array("uvTiling");
  writer.write_float_value(params.uvTiling.x);
  writer.write_float_value(params.uvTiling.y);
  writer.end_array();

  writer.begin_array("uvOffset");
  writer.write_float_value(params.uvOffset.x);
  writer.write_float_value(params.uvOffset.y);
  writer.end_array();

  const bool hasAnyTexture =
      (textureSlots.albedo != kInvalidAssetId) ||
      (textureSlots.metallicRoughness != kInvalidAssetId) ||
      (textureSlots.emissive != kInvalidAssetId) ||
      (textureSlots.occlusion != kInvalidAssetId) ||
      (textureSlots.opacity != kInvalidAssetId);
  bool textureSlotsOk = true;
  if (hasAnyTexture) {
    writer.write_key("textures");
    writer.begin_object();
    textureSlotsOk =
        write_texture_slot(&writer, database, "albedo", textureSlots.albedo) &&
        write_texture_slot(&writer, database, "metallicRoughness",
                           textureSlots.metallicRoughness) &&
        write_texture_slot(&writer, database, "emissive",
                           textureSlots.emissive) &&
        write_texture_slot(&writer, database, "occlusion",
                           textureSlots.occlusion) &&
        write_texture_slot(&writer, database, "opacity", textureSlots.opacity);
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
