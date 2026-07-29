// Implements JSON material asset loading with parent-chain (instance)
// resolution for the Engine renderer system.

#include "engine/renderer/material_loader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

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


/// True when material registration can insert or update this ID.
bool material_slot_available(const AssetDatabase &database,
                             AssetId id) noexcept {
  for (std::size_t index = 0U; index < database.materialAssets.size();
       ++index) {
    if (!database.materialOccupied[index] ||
        (database.materialAssets[index].id == id)) {
      return true;
    }
  }
  return false;
}

/// True when metadata registration can insert or update this ID.
bool metadata_slot_available(const AssetDatabase &database,
                             AssetId id) noexcept {
  for (std::size_t index = 0U; index < database.metadata.size(); ++index) {
    if (!database.metadataOccupied[index] ||
        (database.metadata[index].assetId == id)) {
      return true;
    }
  }
  return false;
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

  AssetMetadata metadata{};
  metadata.assetId = id;
  metadata.typeTag = AssetTypeTag::Material;
  write_metadata_path(&metadata.filePath, virtualPath);
  if ((parentId != kInvalidAssetId) &&
      !asset_metadata_add_dependency(&metadata, parentId)) {
    return log_material_error(virtualPath,
                              "material dependency table is full");
  }

  if (!material_slot_available(*database, id)) {
    return log_material_error(virtualPath, "material table is full");
  }
  if (!metadata_slot_available(*database, id)) {
    return log_material_error(virtualPath, "metadata table is full");
  }

  // Both fixed tables were preflighted, so these mutations complete together.
  if (!register_material_asset(database, id, virtualPath, params)) {
    return log_material_error(virtualPath,
                              "material registration unexpectedly failed");
  }
  if (!register_asset_metadata(database, metadata)) {
    return log_material_error(virtualPath,
                              "metadata registration unexpectedly failed");
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

std::expected<AssetId, MaterialLoadError>
load_material_asset(AssetDatabase *database, const char *virtualPath) noexcept {
  if ((database == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    static_cast<void>(log_material_error(virtualPath, "invalid arguments"));
    return std::unexpected(MaterialLoadError::InvalidArgument);
  }

  const AssetId id = make_asset_id_from_path(virtualPath);
  if (find_material_params(database, id) != nullptr) {
    return id;
  }

  char *text = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_text(virtualPath, &text, &size)) {
    static_cast<void>(log_material_error(virtualPath, "failed to read file"));
    return std::unexpected(MaterialLoadError::Io);
  }

  const bool loaded =
      parse_material_text(database, virtualPath, text, size, 0U, id, nullptr);
  core::vfs_free(text);
  if (!loaded) {
    return std::unexpected(MaterialLoadError::Parse);
  }
  return id;
}

std::size_t load_material_assets_in_directory(
    AssetDatabase *database, const char *osDirectory,
    const char *virtualPrefix) noexcept {
  if ((database == nullptr) || (osDirectory == nullptr) ||
      (virtualPrefix == nullptr)) {
    return 0U;
  }

  // Collect *.json names into fixed storage, then sort so registration
  // order (and therefore record slot layout) is deterministic.
  constexpr std::size_t kMaxDiscovered = 256U;
  constexpr std::size_t kMaxNameLength = 128U;
  static std::array<std::array<char, kMaxNameLength>, kMaxDiscovered> names{};
  std::size_t nameCount = 0U;

  std::error_code error{};
  std::filesystem::directory_iterator it(osDirectory, error);
  if (error) {
    return 0U;
  }
  for (const std::filesystem::directory_entry &entry : it) {
    if (!entry.is_regular_file(error) || error) {
      continue;
    }
    const std::filesystem::path &path = entry.path();
    if (path.extension() != ".json") {
      continue;
    }
    if (nameCount >= kMaxDiscovered) {
      log_material_error(osDirectory, "too many material files; rest skipped");
      break;
    }
    const std::string fileName = path.filename().string();
    if (fileName.size() >= kMaxNameLength) {
      log_material_error(fileName.c_str(), "material file name too long");
      continue;
    }
    std::memcpy(names[nameCount].data(), fileName.c_str(),
                fileName.size() + 1U);
    ++nameCount;
  }

  std::sort(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(nameCount),
            [](const std::array<char, kMaxNameLength> &lhs,
               const std::array<char, kMaxNameLength> &rhs) noexcept {
              return std::strcmp(lhs.data(), rhs.data()) < 0;
            });

  std::size_t loaded = 0U;
  for (std::size_t i = 0U; i < nameCount; ++i) {
    char virtualPath[512] = {};
    std::snprintf(virtualPath, sizeof(virtualPath), "%s/%s", virtualPrefix,
                  names[i].data());
    if (load_material_asset(database, virtualPath).has_value()) {
      ++loaded;
    }
  }
  return loaded;
}

} // namespace engine::renderer
