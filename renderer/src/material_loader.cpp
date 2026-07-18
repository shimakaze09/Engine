// Implements JSON material asset loading with parent-chain (instance)
// resolution for the Engine renderer system.

#include "engine/renderer/material_loader.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/math/vec3.h"

namespace engine::renderer {

namespace {

constexpr const char *kMaterialLogChannel = "material";
constexpr std::uint32_t kSupportedMaterialVersion = 1U;

/// Logs a material load failure with the offending path; always false.
bool log_material_error(const char *virtualPath, const char *message) noexcept {
  char buffer[512] = {};
  std::snprintf(buffer, sizeof(buffer), "%s: %s",
                (virtualPath != nullptr) ? virtualPath : "<null>",
                (message != nullptr) ? message : "unknown error");
  core::log_message(core::LogLevel::Error, kMaterialLogChannel, buffer);
  return false;
}

/// Reads an optional Vec3 field; strict when present, untouched when absent.
bool read_optional_vec3(const core::JsonParser &parser,
                        const core::JsonValue &object, const char *key,
                        math::Vec3 *outValue) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, key, &field)) {
    return true;
  }

  float components[3] = {};
  if (!parser.as_float_array(field, components, 3U)) {
    return false;
  }

  *outValue = math::Vec3(components[0], components[1], components[2]);
  return true;
}

/// Reads an optional float field; strict when present, untouched when absent.
bool read_optional_float(const core::JsonParser &parser,
                         const core::JsonValue &object, const char *key,
                         float *outValue) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, key, &field)) {
    return true;
  }

  return parser.as_float(field, outValue);
}

bool load_material_recursive(AssetDatabase *database, const char *virtualPath,
                             std::size_t depth, AssetId *outId,
                             Material *outParams) noexcept;

/// Parses one material file's JSON text and registers the resolved record.
/// The text buffer must stay alive for the whole call: JsonValues reference
/// slices of it.
bool parse_material_text(AssetDatabase *database, const char *virtualPath,
                         const char *text, std::size_t size, std::size_t depth,
                         AssetId id, Material *outParams) noexcept {
  core::JsonParser parser{};
  if (!parser.parse(text, size)) {
    return log_material_error(virtualPath, "malformed JSON");
  }

  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    return log_material_error(virtualPath, "root must be an object");
  }

  core::JsonValue versionValue{};
  if (parser.get_object_field(*root, "version", &versionValue)) {
    std::uint32_t version = 0U;
    if (!parser.as_uint(versionValue, &version) ||
        (version != kSupportedMaterialVersion)) {
      return log_material_error(virtualPath, "unsupported material version");
    }
  }

  // Start from defaults, or from the parent's fully resolved parameters.
  Material params{};
  AssetId parentId = kInvalidAssetId;
  core::JsonValue parentValue{};
  if (parser.get_object_field(*root, "parent", &parentValue)) {
    char parentPath[260] = {};
    if (!parser.copy_string(parentValue, parentPath, sizeof(parentPath)) ||
        (parentPath[0] == '\0')) {
      return log_material_error(virtualPath, "invalid parent path");
    }
    if (!load_material_recursive(database, parentPath, depth + 1U, &parentId,
                                 &params)) {
      return log_material_error(virtualPath, "failed to load parent");
    }
  }

  if (!read_optional_vec3(parser, *root, "albedo", &params.albedo) ||
      !read_optional_vec3(parser, *root, "emissive", &params.emissive) ||
      !read_optional_float(parser, *root, "roughness", &params.roughness) ||
      !read_optional_float(parser, *root, "metallic", &params.metallic) ||
      !read_optional_float(parser, *root, "opacity", &params.opacity)) {
    return log_material_error(virtualPath, "malformed parameter field");
  }

  if (!register_material_asset(database, id, virtualPath, params)) {
    return log_material_error(virtualPath, "material table is full");
  }

  AssetMetadata metadata{};
  metadata.assetId = id;
  metadata.typeTag = AssetTypeTag::Material;
  write_metadata_path(&metadata.filePath, virtualPath);
  if (parentId != kInvalidAssetId) {
    static_cast<void>(asset_metadata_add_dependency(&metadata, parentId));
  }
  static_cast<void>(register_asset_metadata(database, metadata));
  if (parentId != kInvalidAssetId) {
    static_cast<void>(add_asset_dependency(database, id, parentId));
  }

  if (outParams != nullptr) {
    *outParams = params;
  }
  return true;
}

/// Loads one material file, recursing into its parent first so overrides
/// apply on top of the parent's resolved values.
bool load_material_recursive(AssetDatabase *database, const char *virtualPath,
                             std::size_t depth, AssetId *outId,
                             Material *outParams) noexcept {
  if ((database == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    return log_material_error(virtualPath, "invalid arguments");
  }

  if (depth >= kMaxMaterialParentDepth) {
    return log_material_error(virtualPath,
                              "parent chain too deep (cycle or depth > 8)");
  }

  const AssetId id = make_asset_id_from_path(virtualPath);

  // Already resolved (shared parents load once).
  if (const Material *cached = find_material_params(database, id)) {
    if (outId != nullptr) {
      *outId = id;
    }
    if (outParams != nullptr) {
      *outParams = *cached;
    }
    return true;
  }

  char *text = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_text(virtualPath, &text, &size)) {
    return log_material_error(virtualPath, "failed to read file");
  }

  const bool loaded =
      parse_material_text(database, virtualPath, text, size, depth, id,
                          outParams);
  core::vfs_free(text);

  if (loaded && (outId != nullptr)) {
    *outId = id;
  }
  return loaded;
}

} // namespace

bool load_material_asset(AssetDatabase *database, const char *virtualPath,
                         AssetId *outId) noexcept {
  return load_material_recursive(database, virtualPath, 0U, outId, nullptr);
}

} // namespace engine::renderer
