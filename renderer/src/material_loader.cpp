// Implements JSON material asset loading with parent-chain (instance)
// resolution and texture-handle resolution for the Engine renderer system.

#include "engine/renderer/material_loader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/core/diagnostic.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/math/vec2.h"
#include "engine/math/vec3.h"
#include "engine/renderer/material_inheritance.h"

namespace engine::renderer {

namespace {

constexpr const char *kMaterialLogChannel = "material";
/// The one material revision this build reads. An older revision is
/// refused rather than migrated: the project is unreleased, so the tree
/// migrates once per format change instead of carrying a read path per
/// past revision. A reader that guessed would drop the fields it no
/// longer knows and resave the material as a reduction of itself.
constexpr std::uint32_t kMaterialVersion = 3U;

/// Logs a material load failure with the offending path; always false.
bool log_material_error(const char *virtualPath, const char *message) noexcept {
  core::log_path_diagnostic(core::LogLevel::Error, kMaterialLogChannel,
                            (virtualPath != nullptr) ? virtualPath : "<null>",
                            message);
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

/// Reads an optional Vec2 field; strict when present, untouched when absent.
bool read_optional_vec2(const core::JsonParser &parser,
                        const core::JsonValue &object, const char *key,
                        math::Vec2 *outValue) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, key, &field)) {
    return true;
  }

  float components[2] = {};
  if (!parser.as_float_array(field, components, 2U)) {
    return false;
  }

  *outValue = math::Vec2(components[0], components[1]);
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

/// Reads an optional "alphaMode" string field ("opaque"/"mask"/"blend");
/// strict when present (unknown text rejects the load), untouched when
/// absent.
bool read_optional_alpha_mode(const core::JsonParser &parser,
                              const core::JsonValue &object,
                              AlphaMode *outValue) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, "alphaMode", &field)) {
    return true;
  }

  char text[16] = {};
  if (!parser.copy_string(field, text, sizeof(text))) {
    return false;
  }

  if (std::strcmp(text, "opaque") == 0) {
    *outValue = AlphaMode::Opaque;
  } else if (std::strcmp(text, "mask") == 0) {
    *outValue = AlphaMode::Mask;
  } else if (std::strcmp(text, "blend") == 0) {
    *outValue = AlphaMode::Blend;
  } else {
    return false;
  }
  return true;
}

/// Reads the optional shadingModel field. Absent keeps the caller's
/// value, which inherits the parent's where a material has one and is
/// physically-based otherwise. A present-but-unknown name refuses the
/// load: silently lighting a surface by a model the author did not ask
/// for is a wrong picture, not a default.
bool read_optional_shading_model(const core::JsonParser &parser,
                                 const core::JsonValue &object,
                                 ShadingModel *outValue) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, "shadingModel", &field)) {
    return true;
  }

  char text[16] = {};
  if (!parser.copy_string(field, text, sizeof(text))) {
    return false;
  }

  if (std::strcmp(text, "pbr") == 0) {
    *outValue = ShadingModel::Pbr;
  } else if (std::strcmp(text, "toon") == 0) {
    *outValue = ShadingModel::Toon;
  } else if (std::strcmp(text, "unlit") == 0) {
    *outValue = ShadingModel::Unlit;
  } else {
    return false;
  }
  return true;
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
  const content::MetadataStore &store = database.metadataStore;
  for (std::size_t index = 0U; index < store.entries.size(); ++index) {
    if (!store.occupied[index] || (store.entries[index].assetId == id)) {
      return true;
    }
  }
  return false;
}

/// Reads an optional texture-slot path field from a v2 "textures" object:
/// strict when present, keeps the parent-inherited slot id when absent
/// (matching every other override field in this schema). A present path
/// registers (or reuses) Texture-tagged metadata for it and records a
/// dependency edge on the owning material's in-progress metadata record.
/// Texture metadata registered here during an overall load that later fails
/// (e.g. the material's own table is full) is not rolled back — the same
/// property the parent-material dependency load above already has.
bool read_optional_texture_ref(const core::JsonParser &parser,
                               const core::JsonValue &object, const char *key,
                               AssetDatabase *database, AssetMetadata *metadata,
                               AssetId *outId) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, key, &field)) {
    return true;
  }

  char texturePath[260] = {};
  if (!parser.copy_string(field, texturePath, sizeof(texturePath)) ||
      (texturePath[0] == '\0')) {
    return false;
  }

  const AssetId textureId = make_asset_id_from_path(texturePath);
  if (find_asset_metadata(database, textureId) == nullptr) {
    if (!metadata_slot_available(*database, textureId)) {
      return false;
    }
    AssetMetadata textureMetadata{};
    textureMetadata.assetId = textureId;
    textureMetadata.typeTag = AssetTypeTag::Texture;
    write_metadata_path(&textureMetadata.filePath, texturePath);
    if (!register_asset_metadata(database, textureMetadata)) {
      return false;
    }
  }

  if (!asset_metadata_add_dependency(metadata, textureId)) {
    return false;
  }

  *outId = textureId;
  return true;
}

bool load_material_recursive(AssetDatabase *database, const char *virtualPath,
                             std::size_t depth, AssetId *outId,
                             Material *outParams,
                             MaterialTextureSlots *outSlots) noexcept;

/// material_field bits for the fields the document itself names; the rest
/// its parent supplies.
std::uint16_t authored_fields(const core::JsonParser &parser,
                              const core::JsonValue &root) noexcept {
  struct Field final {
    const char *key;
    std::uint16_t bit;
  };
  static constexpr Field kFields[] = {
      {"albedo", material_field::kAlbedo},
      {"emissive", material_field::kEmissive},
      {"roughness", material_field::kRoughness},
      {"metallic", material_field::kMetallic},
      {"opacity", material_field::kOpacity},
      {"shadingModel", material_field::kShadingModel},
      {"alphaMode", material_field::kAlphaMode},
      {"alphaCutoff", material_field::kAlphaCutoff},
      {"uvTiling", material_field::kUvTiling},
      {"uvOffset", material_field::kUvOffset},
  };
  static constexpr Field kTextureFields[] = {
      {"albedo", material_field::kAlbedoTexture},
      {"metallicRoughness", material_field::kMetallicRoughnessTexture},
      {"emissive", material_field::kEmissiveTexture},
      {"occlusion", material_field::kOcclusionTexture},
      {"opacity", material_field::kOpacityTexture},
  };

  std::uint16_t authored = 0U;
  core::JsonValue value{};
  for (const Field &field : kFields) {
    if (parser.get_object_field(root, field.key, &value)) {
      authored = static_cast<std::uint16_t>(authored | field.bit);
    }
  }
  core::JsonValue textures{};
  if (parser.get_object_field(root, "textures", &textures)) {
    for (const Field &field : kTextureFields) {
      if (parser.get_object_field(textures, field.key, &value)) {
        authored = static_cast<std::uint16_t>(authored | field.bit);
      }
    }
  }
  return authored;
}

/// Parses one material file's JSON text and registers the resolved record;
/// both fixed tables are preflighted for space first so the two mutations
/// complete together. The text buffer must stay alive for the whole call:
/// JsonValues reference slices of it. Every database mutation happens after
/// every validation step below has already succeeded (the two
/// slot-availability checks are last), so a parse failure at any point
/// leaves a previously registered record for `id` completely untouched —
/// the property reload_material_asset relies on.
bool parse_material_text(AssetDatabase *database, const char *virtualPath,
                         const char *text, std::size_t size, std::size_t depth,
                         AssetId id, Material *outParams,
                         MaterialTextureSlots *outSlots) noexcept {
  core::JsonParser parser{};
  if (!parser.parse(text, size)) {
    return log_material_error(virtualPath, "malformed JSON");
  }

  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    return log_material_error(virtualPath, "root must be an object");
  }

  // Exactly one revision loads, and a file naming none names no revision
  // at all, so it is refused with the rest.
  std::uint32_t version = 0U;
  core::JsonValue versionValue{};
  if (parser.get_object_field(*root, "version", &versionValue) &&
      !parser.as_uint(versionValue, &version)) {
    return log_material_error(virtualPath, "material version is not a number");
  }
  if (version != kMaterialVersion) {
    return log_material_error(virtualPath, "unsupported material version");
  }

  Material params{};
  MaterialTextureSlots slots{};
  AssetId parentId = kInvalidAssetId;
  core::JsonValue parentValue{};
  if (parser.get_object_field(*root, "parent", &parentValue)) {
    char parentPath[260] = {};
    if (!parser.copy_string(parentValue, parentPath, sizeof(parentPath)) ||
        (parentPath[0] == '\0')) {
      return log_material_error(virtualPath, "invalid parent path");
    }
    if (!load_material_recursive(database, parentPath, depth + 1U, &parentId,
                                 &params, &slots)) {
      return log_material_error(virtualPath, "failed to load parent");
    }
    // A parent already loaded skips the depth walk above, so a reload that
    // names one of its own descendants is caught here instead.
    if (material_chain_contains(database, parentId, id)) {
      return log_material_error(virtualPath,
                                "parent chain would become a cycle");
    }
  }
  // Texture GPU handles are never inherited directly: they are re-derived
  // by resolve_material_textures from `slots` every sync, so a slot that
  // this file overrides (below) cannot keep showing a stale parent texture.
  params.albedoTexture = kInvalidTextureHandle;
  params.metallicRoughnessTexture = kInvalidTextureHandle;
  params.emissiveTexture = kInvalidTextureHandle;
  params.occlusionTexture = kInvalidTextureHandle;
  params.opacityTexture = kInvalidTextureHandle;

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
  // Registering this record replaces whatever the catalog holds for this
  // path, and the mount walk got there first and resolved the identity
  // this material's sidecar authored. Carry that identity forward: a
  // document names a material by its GUID, so dropping it here would
  // leave every authored reference pointing at nothing, and a save would
  // write back the nil ref the entity was left holding.
  if (const AssetMetadata *catalogued = find_asset_metadata(database, id);
      catalogued != nullptr) {
    metadata.ref = catalogued->ref;
  }
  write_metadata_path(&metadata.filePath, virtualPath);
  if ((parentId != kInvalidAssetId) &&
      !asset_metadata_add_dependency(&metadata, parentId)) {
    return log_material_error(virtualPath,
                              "material dependency table is full");
  }

  if (!read_optional_shading_model(parser, *root, &params.shadingModel) ||
      !read_optional_alpha_mode(parser, *root, &params.alphaMode) ||
      !read_optional_float(parser, *root, "alphaCutoff",
                           &params.alphaCutoff) ||
      !read_optional_vec2(parser, *root, "uvTiling", &params.uvTiling) ||
      !read_optional_vec2(parser, *root, "uvOffset", &params.uvOffset)) {
    return log_material_error(virtualPath, "malformed parameter field");
  }

  core::JsonValue texturesValue{};
  if (parser.get_object_field(*root, "textures", &texturesValue)) {
    if (texturesValue.type != core::JsonValue::Type::Object) {
      return log_material_error(virtualPath, "textures must be an object");
    }
    if (!read_optional_texture_ref(parser, texturesValue, "albedo", database,
                                   &metadata, &slots.albedo) ||
        !read_optional_texture_ref(parser, texturesValue,
                                   "metallicRoughness", database, &metadata,
                                   &slots.metallicRoughness) ||
        !read_optional_texture_ref(parser, texturesValue, "emissive",
                                   database, &metadata, &slots.emissive) ||
        !read_optional_texture_ref(parser, texturesValue, "occlusion",
                                   database, &metadata, &slots.occlusion) ||
        !read_optional_texture_ref(parser, texturesValue, "opacity",
                                   database, &metadata, &slots.opacity)) {
      return log_material_error(virtualPath, "malformed texture reference");
    }
  }

  const std::uint16_t overridden = authored_fields(parser, *root);

  if (!material_slot_available(*database, id)) {
    return log_material_error(virtualPath, "material table is full");
  }
  if (!metadata_slot_available(*database, id)) {
    return log_material_error(virtualPath, "metadata table is full");
  }

  if (!register_material_asset(database, id, virtualPath, params)) {
    return log_material_error(virtualPath,
                              "material registration unexpectedly failed");
  }
  if (!register_asset_metadata(database, metadata)) {
    return log_material_error(virtualPath,
                              "metadata registration unexpectedly failed");
  }
  if (!set_material_texture_slots(database, id, slots) ||
      !set_material_overrides(database, id, overridden)) {
    return log_material_error(virtualPath,
                              "material record update unexpectedly failed");
  }

  if (outParams != nullptr) {
    *outParams = params;
  }
  if (outSlots != nullptr) {
    *outSlots = slots;
  }
  return true;
}

/// Loads one material file, recursing into its parent first so overrides
/// apply on top of the parent's resolved values.
bool load_material_recursive(AssetDatabase *database, const char *virtualPath,
                             std::size_t depth, AssetId *outId,
                             Material *outParams,
                             MaterialTextureSlots *outSlots) noexcept {
  if ((database == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    return log_material_error(virtualPath, "invalid arguments");
  }

  if (depth >= kMaxMaterialParentDepth) {
    return log_material_error(virtualPath,
                              "parent chain too deep (cycle or depth > 8)");
  }

  const AssetId id = make_asset_id_from_path(virtualPath);

  if (const Material *cached = find_material_params(database, id)) {
    if (outId != nullptr) {
      *outId = id;
    }
    if (outParams != nullptr) {
      *outParams = *cached;
    }
    if (outSlots != nullptr) {
      const MaterialTextureSlots *cachedSlots =
          find_material_texture_slots(database, id);
      *outSlots =
          (cachedSlots != nullptr) ? *cachedSlots : MaterialTextureSlots{};
    }
    return true;
  }

  char *text = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_text(virtualPath, &text, &size)) {
    return log_material_error(virtualPath, "failed to read file");
  }

  const bool loaded = parse_material_text(database, virtualPath, text, size,
                                          depth, id, outParams, outSlots);
  core::vfs_free(text);

  if (loaded && (outId != nullptr)) {
    *outId = id;
  }
  return loaded;
}

/// What resolving one texture slot did.
enum class SlotOutcome : std::uint8_t {
  /// Nothing new: an empty slot, a lookup, or a failure now recorded.
  Unchanged,
  /// This call promoted the id to Ready.
  Resolved,
  /// The texture table has no room to record the id, Ready or Failed, so
  /// the slot must not be tried again: a load whose result cannot be
  /// recorded is repeated on every call, uploading and leaking a device
  /// texture each time.
  Unregisterable,
};

/// Resolves one texture slot's AssetId into a material's TextureHandle
/// field. An already-Ready or already-Failed id is a cheap lookup, never a
/// reload, and nothing is loaded while there is no table slot to record it.
SlotOutcome resolve_one_texture_slot(AssetDatabase *database, AssetId textureId,
                                     MaterialTextureLoadFn loadFn,
                                     void *userData,
                                     TextureHandle *outHandle) noexcept {
  *outHandle = kInvalidTextureHandle;
  if (textureId == kInvalidAssetId) {
    return SlotOutcome::Unchanged;
  }

  const AssetState state = texture_asset_state(database, textureId);
  if (state == AssetState::Ready) {
    *outHandle = resolve_texture_asset(database, textureId);
    return SlotOutcome::Unchanged;
  }
  if (state == AssetState::Failed) {
    return SlotOutcome::Unchanged;
  }

  const AssetMetadata *metadata = find_asset_metadata(database, textureId);
  const char *path = ((metadata != nullptr) && (metadata->filePath[0] != '\0'))
                         ? metadata->filePath.data()
                         : nullptr;
  if (!texture_asset_slot_available(database, textureId)) {
    char message[512] = {};
    std::snprintf(message, sizeof(message),
                  "texture table is full (%zu textures); material falls back "
                  "to its scalar parameters: %s",
                  database->textureAssets.size(),
                  (path != nullptr) ? path : "(no source path)");
    core::log_message(core::LogLevel::Error, kMaterialLogChannel, message);
    return SlotOutcome::Unregisterable;
  }
  if ((path == nullptr) || (loadFn == nullptr)) {
    static_cast<void>(register_texture_asset_failed(database, textureId, path));
    *outHandle = kInvalidTextureHandle;
    core::log_message(
        core::LogLevel::Error, kMaterialLogChannel,
        "material texture reference has no resolvable source path");
    return SlotOutcome::Unchanged;
  }

  const TextureHandle loaded = loadFn(path, userData);
  if (loaded == kInvalidTextureHandle) {
    static_cast<void>(register_texture_asset_failed(database, textureId, path));
    char message[512] = {};
    std::snprintf(message, sizeof(message),
                 "material texture failed to load; material falls back to "
                 "its scalar parameters: %s",
                 path);
    core::log_message(core::LogLevel::Error, kMaterialLogChannel, message);
    return SlotOutcome::Unchanged;
  }

  // Cannot fail: the slot was checked above and nothing ran in between.
  static_cast<void>(register_texture_asset(database, textureId, path, loaded));
  *outHandle = loaded;
  return SlotOutcome::Resolved;
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

  const bool loaded = parse_material_text(database, virtualPath, text, size,
                                          0U, id, nullptr, nullptr);
  core::vfs_free(text);
  if (!loaded) {
    return std::unexpected(MaterialLoadError::Parse);
  }
  return id;
}

std::expected<AssetId, MaterialLoadError>
reload_material_asset(AssetDatabase *database,
                      const char *virtualPath) noexcept {
  if ((database == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    static_cast<void>(log_material_error(virtualPath, "invalid arguments"));
    return std::unexpected(MaterialLoadError::InvalidArgument);
  }

  const AssetId id = make_asset_id_from_path(virtualPath);
  if (material_asset_state(database, id) != AssetState::Ready) {
    static_cast<void>(log_material_error(
        virtualPath, "reload requested for a material that was never loaded"));
    return std::unexpected(MaterialLoadError::InvalidArgument);
  }

  char *text = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_text(virtualPath, &text, &size)) {
    static_cast<void>(log_material_error(virtualPath, "failed to read file"));
    return std::unexpected(MaterialLoadError::Io);
  }

  const bool loaded = parse_material_text(database, virtualPath, text, size,
                                          0U, id, nullptr, nullptr);
  core::vfs_free(text);
  if (!loaded) {
    // parse_material_text never mutated the database (see its header
    // comment): the previously Ready record is exactly as it was.
    return std::unexpected(MaterialLoadError::Parse);
  }
  static_cast<void>(propagate_material_to_dependents(database, id));
  return id;
}

std::size_t load_material_assets_in_directory(
    AssetDatabase *database, const char *osDirectory,
    const char *virtualPrefix) noexcept {
  if ((database == nullptr) || (osDirectory == nullptr) ||
      (virtualPrefix == nullptr)) {
    return 0U;
  }

  // Discovered names are sorted before registration so record slot
  // layout is deterministic across platforms and directory orders.
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
    if (path.extension() != ".mat") {
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

std::size_t resolve_material_textures(AssetDatabase *database,
                                      MaterialTextureLoadFn loadFn,
                                      void *userData) noexcept {
  if (database == nullptr) {
    return 0U;
  }

  std::size_t resolvedCount = 0U;
  for (std::size_t i = 0U; i < database->materialAssets.size(); ++i) {
    if (!database->materialOccupied[i]) {
      continue;
    }
    MaterialAssetRecord &record = database->materialAssets[i];
    if (record.state != AssetState::Ready) {
      continue;
    }

    const MaterialTextureSlots slots = record.textureSlots;
    struct SlotRef final {
      AssetId id;
      TextureHandle *handle;
    };
    const SlotRef refs[] = {
        {slots.albedo, &record.params.albedoTexture},
        {slots.metallicRoughness, &record.params.metallicRoughnessTexture},
        {slots.emissive, &record.params.emissiveTexture},
        {slots.occlusion, &record.params.occlusionTexture},
        {slots.opacity, &record.params.opacityTexture},
    };
    static_assert(sizeof(refs) / sizeof(refs[0]) <= 8U,
                  "unregisterableTextureSlots holds one bit per slot");
    for (std::size_t slot = 0U; slot < sizeof(refs) / sizeof(refs[0]); ++slot) {
      const auto bit = static_cast<std::uint8_t>(1U << slot);
      if ((record.unregisterableTextureSlots & bit) != 0U) {
        continue;
      }
      switch (resolve_one_texture_slot(database, refs[slot].id, loadFn,
                                       userData, refs[slot].handle)) {
      case SlotOutcome::Resolved:
        ++resolvedCount;
        break;
      case SlotOutcome::Unregisterable:
        record.unregisterableTextureSlots |= bit;
        break;
      case SlotOutcome::Unchanged:
        break;
      }
    }
  }
  return resolvedCount;
}

} // namespace engine::renderer
