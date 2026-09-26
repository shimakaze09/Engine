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

#include "engine/content/asset_identity.h"
#include "engine/content/asset_ref_json.h"
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
                              const core::JsonValue &object, const char *key,
                              AlphaMode *outValue) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, key, &field)) {
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
                                 const core::JsonValue &object, const char *key,
                                 ShadingModel *outValue) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, key, &field)) {
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

/// One overload per material field type, so the field table can read
/// every field through one name.
bool read_field(const core::JsonParser &parser, const core::JsonValue &object,
                const char *key, math::Vec3 *out) noexcept {
  return read_optional_vec3(parser, object, key, out);
}
bool read_field(const core::JsonParser &parser, const core::JsonValue &object,
                const char *key, math::Vec2 *out) noexcept {
  return read_optional_vec2(parser, object, key, out);
}
bool read_field(const core::JsonParser &parser, const core::JsonValue &object,
                const char *key, float *out) noexcept {
  return read_optional_float(parser, object, key, out);
}
bool read_field(const core::JsonParser &parser, const core::JsonValue &object,
                const char *key, AlphaMode *out) noexcept {
  return read_optional_alpha_mode(parser, object, key, out);
}
bool read_field(const core::JsonParser &parser, const core::JsonValue &object,
                const char *key, ShadingModel *out) noexcept {
  return read_optional_shading_model(parser, object, key, out);
}

/// Reads the reference at `value` and finds the catalogued asset it names.
/// Null, logged against the material, when the text is not a reference,
/// when the catalog has no asset by that identity, or when the asset it
/// names is not of `type`: a material that loaded with the reference
/// quietly gone would save back without it.
const content::AssetMetadata *read_catalogued_ref(
    const core::JsonParser &parser, const core::JsonValue &value,
    const content::AssetCatalog &catalog, content::AssetTypeTag type,
    const char *virtualPath, const char *what) noexcept {
  core::AssetRef ref{};
  char message[160] = {};
  if (!content::read_asset_ref(parser, value, &ref)) {
    std::snprintf(message, sizeof(message), "%s is not an asset reference",
                  what);
    static_cast<void>(log_material_error(virtualPath, message));
    return nullptr;
  }
  const content::AssetMetadata *record =
      content::find_asset_metadata_by_ref(&catalog, ref);
  if (record == nullptr) {
    char refText[content::kAssetRefTextLength + 1U] = {};
    static_cast<void>(content::format_asset_ref(ref, refText, sizeof(refText)));
    std::snprintf(message, sizeof(message), "%s names no catalogued asset: %s",
                  what, refText);
    static_cast<void>(log_material_error(virtualPath, message));
    return nullptr;
  }
  if (record->typeTag != type) {
    std::snprintf(message, sizeof(message), "%s names an asset of another type",
                  what);
    static_cast<void>(log_material_error(virtualPath, message));
    return nullptr;
  }
  return record;
}

/// Reads an optional texture slot from the "textures" object: strict when
/// present, keeps the parent-inherited slot id when absent (matching every
/// other override field in this schema). A present slot is the texture's
/// reference, resolved through the catalog to the id this session knows
/// the texture by, and recorded as a dependency edge on the owning
/// material's in-progress metadata record.
bool read_optional_texture_ref(const core::JsonParser &parser,
                               const core::JsonValue &object, const char *key,
                               bool hasParent,
                               const content::AssetCatalog &catalog,
                               const char *virtualPath,
                               content::AssetMetadata *metadata,
                               content::AssetId *outId) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, key, &field)) {
    return true;
  }
  // null clears a slot the parent would otherwise supply. Only a child
  // has anything to clear: in a root material an absent slot is already
  // empty, so null there is not a second spelling of it but a mistake.
  if (field.type == core::JsonValue::Type::Null) {
    if (!hasParent) {
      return log_material_error(virtualPath,
                                "null texture slot in a material with no "
                                "parent");
    }
    *outId = content::kInvalidAssetId;
    return true;
  }

  const content::AssetMetadata *texture = read_catalogued_ref(
      parser, field, catalog, content::AssetTypeTag::Texture, virtualPath, key);
  if (texture == nullptr) {
    return false;
  }
  if (!content::asset_metadata_add_dependency(metadata, texture->assetId)) {
    return log_material_error(virtualPath, "material dependency table is full");
  }

  *outId = texture->assetId;
  return true;
}

bool load_material_recursive(AssetDatabase *database,
                             content::AssetCatalog *catalog,
                             const char *virtualPath, std::size_t depth,
                             content::AssetId *outId, Material *outParams,
                             MaterialTextureSlots *outSlots) noexcept;

bool validate_material_file(AssetDatabase *database,
                            content::AssetCatalog *catalog,
                            const char *virtualPath,
                            std::size_t depth) noexcept;

/// material_field bits for the fields the document itself names; the rest
/// its parent supplies.
std::uint16_t authored_fields(const core::JsonParser &parser,
                              const core::JsonValue &root) noexcept {
  std::uint16_t authored = 0U;
  core::JsonValue value{};
#define ENGINE_MATERIAL_AUTHORED_PARAM(name, member, key)                      \
  if (parser.get_object_field(root, key, &value)) {                            \
    authored = static_cast<std::uint16_t>(authored | material_field::k##name); \
  }
  ENGINE_MATERIAL_PARAM_FIELDS(ENGINE_MATERIAL_AUTHORED_PARAM)
#undef ENGINE_MATERIAL_AUTHORED_PARAM
  core::JsonValue textures{};
  if (parser.get_object_field(root, "textures", &textures)) {
#define ENGINE_MATERIAL_AUTHORED_TEXTURE(name, slot, handle, key)              \
  if (parser.get_object_field(textures, key, &value)) {                        \
    authored = static_cast<std::uint16_t>(authored | material_field::k##name); \
  }
    ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_AUTHORED_TEXTURE)
#undef ENGINE_MATERIAL_AUTHORED_TEXTURE
  }
  return authored;
}

/// Parses one material file's JSON text and registers the resolved record;
/// both fixed tables are preflighted for space first so the two mutations
/// complete together. The text buffer must stay alive for the whole call:
/// JsonValues reference slices of it. Every mutation happens after
/// every validation step below has already succeeded (the two
/// slot-availability checks are last), so a parse failure at any point
/// leaves a previously registered record for `id` completely untouched.
///
/// With `commit` false it only validates: a parent that is not loaded is
/// validated from its file rather than loaded, and nothing is registered,
/// so reload_material_asset can prove the whole document good before the
/// commit pass loads anything.
bool parse_material_text(AssetDatabase *database,
                         content::AssetCatalog *catalog,
                         const char *virtualPath, const char *text,
                         std::size_t size, std::size_t depth,
                         content::AssetId id, Material *outParams,
                         MaterialTextureSlots *outSlots,
                         bool commit = true) noexcept {
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
  if (version != kMaterialDocumentVersion) {
    return log_material_error(virtualPath, "unsupported material version");
  }

  Material params{};
  MaterialTextureSlots slots{};
  content::AssetId parentId = content::kInvalidAssetId;
  core::JsonValue parentValue{};
  if (parser.get_object_field(*root, "parent", &parentValue)) {
    const content::AssetMetadata *parent = read_catalogued_ref(
        parser, parentValue, *catalog, content::AssetTypeTag::Material,
        virtualPath, "parent");
    if (parent == nullptr) {
      return false;
    }
    // Loading the parent rewrites its catalog record in place, so the
    // path is copied out rather than read through a record being replaced.
    char parentPath[sizeof(parent->filePath)] = {};
    std::memcpy(parentPath, parent->filePath.data(), sizeof(parentPath));
    const bool parentLoaded =
        find_material_params(database, parent->assetId) != nullptr;
    if (!commit && !parentLoaded) {
      // Validation only: the parent must be loadable, but loading it is
      // the commit pass's to do. Its values do not matter to validity.
      if ((depth + 1U) >= kMaxMaterialParentDepth) {
        return log_material_error(virtualPath,
                                  "parent chain too deep (cycle or depth > 8)");
      }
      if (!validate_material_file(database, catalog, parentPath, depth + 1U)) {
        return log_material_error(virtualPath, "failed to load parent");
      }
      parentId = parent->assetId;
      if (parentId == id) {
        return log_material_error(virtualPath,
                                  "parent chain would become a cycle");
      }
    } else if (!load_material_recursive(database, catalog, parentPath,
                                        depth + 1U, &parentId, &params,
                                        &slots)) {
      return log_material_error(virtualPath, "failed to load parent");
    }
    // A parent already loaded skips the depth walk above, so a reload that
    // names one of its own descendants is caught here instead.
    if (material_chain_contains(catalog, parentId, id)) {
      return log_material_error(virtualPath,
                                "parent chain would become a cycle");
    }
  }
  // Texture GPU handles are never inherited directly: they are re-derived
  // by resolve_material_textures from `slots` every sync, so a slot that
  // this file overrides (below) cannot keep showing a stale parent texture.
#define ENGINE_MATERIAL_CLEAR_HANDLE(name, slot, handle, key)                  \
  params.handle = kInvalidTextureHandle;
  ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_CLEAR_HANDLE)
#undef ENGINE_MATERIAL_CLEAR_HANDLE

  bool fieldsOk = true;
#define ENGINE_MATERIAL_READ_PARAM(name, member, key)                          \
  fieldsOk = fieldsOk && read_field(parser, *root, key, &params.member);
  ENGINE_MATERIAL_PARAM_FIELDS(ENGINE_MATERIAL_READ_PARAM)
#undef ENGINE_MATERIAL_READ_PARAM
  if (!fieldsOk) {
    return log_material_error(virtualPath, "malformed parameter field");
  }

  content::AssetMetadata metadata{};
  metadata.assetId = id;
  metadata.typeTag = content::AssetTypeTag::Material;
  // Registering this record replaces whatever the catalog holds for this
  // path, and the mount walk got there first and resolved the identity
  // this material's sidecar authored. Carry that identity forward: a
  // document names a material by its GUID, so dropping it here would
  // leave every authored reference pointing at nothing, and a save would
  // write back the nil ref the entity was left holding.
  if (const content::AssetMetadata *catalogued =
          find_asset_metadata(catalog, id);
      catalogued != nullptr) {
    metadata.ref = catalogued->ref;
  }
  content::write_metadata_path(&metadata.filePath, virtualPath);
  if ((parentId != content::kInvalidAssetId) &&
      !content::asset_metadata_add_dependency(&metadata, parentId)) {
    return log_material_error(virtualPath,
                              "material dependency table is full");
  }

  core::JsonValue texturesValue{};
  if (parser.get_object_field(*root, "textures", &texturesValue)) {
    if (texturesValue.type != core::JsonValue::Type::Object) {
      return log_material_error(virtualPath, "textures must be an object");
    }
    bool texturesOk = true;
#define ENGINE_MATERIAL_READ_TEXTURE(name, slot, handle, key)                  \
  texturesOk = texturesOk &&                                                   \
               read_optional_texture_ref(parser, texturesValue, key,           \
                                         parentId != content::kInvalidAssetId, \
                                         *catalog, virtualPath, &metadata,     \
                                         &slots.slot);
    ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_READ_TEXTURE)
#undef ENGINE_MATERIAL_READ_TEXTURE
    if (!texturesOk) {
      return false;
    }
  }

  const std::uint16_t overridden = authored_fields(parser, *root);

  if (!material_asset_slot_available(database, id)) {
    return log_material_error(virtualPath, "material table is full");
  }
  if (!can_register_asset_metadata(catalog, id)) {
    return log_material_error(virtualPath, "metadata table is full");
  }
  if (!commit) {
    return true;
  }

  if (!register_material_asset(database, id, virtualPath, params)) {
    return log_material_error(virtualPath,
                              "material registration unexpectedly failed");
  }
  if (!register_asset_metadata(catalog, metadata)) {
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
bool load_material_recursive(AssetDatabase *database,
                             content::AssetCatalog *catalog,
                             const char *virtualPath, std::size_t depth,
                             content::AssetId *outId, Material *outParams,
                             MaterialTextureSlots *outSlots) noexcept {
  if ((database == nullptr) || (catalog == nullptr) ||
      (virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    return log_material_error(virtualPath, "invalid arguments");
  }

  if (depth >= kMaxMaterialParentDepth) {
    return log_material_error(virtualPath,
                              "parent chain too deep (cycle or depth > 8)");
  }

  const content::AssetId id = content::make_asset_id_from_path(virtualPath);

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

  const bool loaded = parse_material_text(database, catalog, virtualPath, text,
                                          size, depth, id, outParams, outSlots);
  core::vfs_free(text);

  if (loaded && (outId != nullptr)) {
    *outId = id;
  }
  return loaded;
}

/// Validates the material file at `virtualPath` without registering
/// anything; see parse_material_text's commit flag.
bool validate_material_file(AssetDatabase *database,
                            content::AssetCatalog *catalog,
                            const char *virtualPath,
                            std::size_t depth) noexcept {
  char *text = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_text(virtualPath, &text, &size)) {
    return log_material_error(virtualPath, "failed to read file");
  }
  const bool valid = parse_material_text(
      database, catalog, virtualPath, text, size, depth,
      content::make_asset_id_from_path(virtualPath), nullptr, nullptr, false);
  core::vfs_free(text);
  return valid;
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
SlotOutcome resolve_one_texture_slot(AssetDatabase *database,
                                     const content::AssetCatalog *catalog,
                                     content::AssetId textureId,
                                     MaterialTextureLoadFn loadFn,
                                     void *userData,
                                     TextureHandle *outHandle) noexcept {
  *outHandle = kInvalidTextureHandle;
  if (textureId == content::kInvalidAssetId) {
    return SlotOutcome::Unchanged;
  }

  const content::AssetState state = texture_asset_state(database, textureId);
  if (state == content::AssetState::Ready) {
    *outHandle = resolve_texture_asset(database, textureId);
    return SlotOutcome::Unchanged;
  }
  if (state == content::AssetState::Failed) {
    return SlotOutcome::Unchanged;
  }

  const content::AssetMetadata *metadata =
      find_asset_metadata(catalog, textureId);
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

  // Read before the load, so a save that lands during it moves the time
  // off the recorded one and the hot-reload poll picks the file up again.
  const std::int64_t writeTime = core::vfs_file_mtime(path);
  const TextureHandle loaded = loadFn(path, userData);
  if (loaded == kInvalidTextureHandle) {
    static_cast<void>(register_texture_asset_failed(database, textureId, path));
    set_texture_source_write_time(database, textureId, writeTime);
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
  set_texture_source_write_time(database, textureId, writeTime);
  *outHandle = loaded;
  return SlotOutcome::Resolved;
}

} // namespace

std::expected<content::AssetId, MaterialLoadError>
load_material_asset(AssetDatabase *database, content::AssetCatalog *catalog,
                    const char *virtualPath) noexcept {
  if ((database == nullptr) || (catalog == nullptr) ||
      (virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    static_cast<void>(log_material_error(virtualPath, "invalid arguments"));
    return std::unexpected(MaterialLoadError::InvalidArgument);
  }

  const content::AssetId id = content::make_asset_id_from_path(virtualPath);
  if (find_material_params(database, id) != nullptr) {
    return id;
  }

  char *text = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_text(virtualPath, &text, &size)) {
    static_cast<void>(log_material_error(virtualPath, "failed to read file"));
    return std::unexpected(MaterialLoadError::Io);
  }

  const bool loaded = parse_material_text(database, catalog, virtualPath, text,
                                          size, 0U, id, nullptr, nullptr);
  core::vfs_free(text);
  if (!loaded) {
    return std::unexpected(MaterialLoadError::Parse);
  }
  return id;
}

std::expected<content::AssetId, MaterialLoadError>
reload_material_asset(AssetDatabase *database, content::AssetCatalog *catalog,
                      const char *virtualPath) noexcept {
  if ((database == nullptr) || (catalog == nullptr) ||
      (virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    static_cast<void>(log_material_error(virtualPath, "invalid arguments"));
    return std::unexpected(MaterialLoadError::InvalidArgument);
  }

  const content::AssetId id = content::make_asset_id_from_path(virtualPath);
  if (material_asset_state(database, id) != content::AssetState::Ready) {
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

  // Prepare and validate the whole document, a parent it newly names
  // included, before anything is loaded or registered; only then commit.
  // A failure in either pass leaves the database and the catalog as they
  // were: the previously Ready record keeps serving.
  const bool valid = parse_material_text(database, catalog, virtualPath, text,
                                         size, 0U, id, nullptr, nullptr, false);
  const bool loaded =
      valid && parse_material_text(database, catalog, virtualPath, text, size,
                                   0U, id, nullptr, nullptr);
  core::vfs_free(text);
  if (!loaded) {
    return std::unexpected(MaterialLoadError::Parse);
  }
  static_cast<void>(content::note_asset_reloaded(catalog, id));
  static_cast<void>(propagate_material_to_dependents(database, catalog, id));
  return id;
}

std::size_t load_material_assets_in_directory(
    AssetDatabase *database, content::AssetCatalog *catalog,
    const char *osDirectory, const char *virtualPrefix) noexcept {
  if ((database == nullptr) || (catalog == nullptr) ||
      (osDirectory == nullptr) || (virtualPrefix == nullptr)) {
    return 0U;
  }

  // Discovered names are sorted before registration so record slot
  // layout is deterministic across platforms and directory orders.
  // The material table is the bound: a name past it could not register.
  constexpr std::size_t kMaxDiscovered = AssetDatabase::kMaxMaterialAssets;
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
    if (load_material_asset(database, catalog, virtualPath).has_value()) {
      ++loaded;
    }
  }
  return loaded;
}

std::size_t resolve_material_textures(AssetDatabase *database,
                                      const content::AssetCatalog *catalog,
                                      MaterialTextureLoadFn loadFn,
                                      void *userData) noexcept {
  if ((database == nullptr) || (catalog == nullptr)) {
    return 0U;
  }

  std::size_t resolvedCount = 0U;
  for (std::size_t i = 0U; i < database->materialAssets.size(); ++i) {
    if (!database->materialOccupied[i]) {
      continue;
    }
    MaterialAssetRecord &record = database->materialAssets[i];
    if (record.state != content::AssetState::Ready) {
      continue;
    }

    const MaterialTextureSlots slots = record.textureSlots;
    struct SlotRef final {
      content::AssetId id;
      TextureHandle *handle;
    };
    const SlotRef refs[] = {
#define ENGINE_MATERIAL_SLOT_REF(name, slot, handle, key)                      \
  {slots.slot, &record.params.handle},
        ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_SLOT_REF)
#undef ENGINE_MATERIAL_SLOT_REF
    };
    static_assert(sizeof(refs) / sizeof(refs[0]) <= 8U,
                  "unregisterableTextureSlots holds one bit per slot");
    for (std::size_t slot = 0U; slot < sizeof(refs) / sizeof(refs[0]); ++slot) {
      const auto bit = static_cast<std::uint8_t>(1U << slot);
      if ((record.unregisterableTextureSlots & bit) != 0U) {
        continue;
      }
      switch (resolve_one_texture_slot(database, catalog, refs[slot].id, loadFn,
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

std::size_t release_unreferenced_textures(AssetDatabase *database,
                                          MaterialTextureReleaseFn releaseFn,
                                          void *userData) noexcept {
  if ((database == nullptr) || !database->textureReferencesChanged) {
    return 0U;
  }
  database->textureReferencesChanged = false;

  std::array<bool, AssetDatabase::kMaxTextureAssets> named{};
  for (std::size_t i = 0U; i < database->materialAssets.size(); ++i) {
    if (!database->materialOccupied[i]) {
      continue;
    }
    const MaterialTextureSlots &slots =
        database->materialAssets[i].textureSlots;
    const content::AssetId ids[] = {
#define ENGINE_MATERIAL_SLOT_ID(name, slot, handle, key) slots.slot,
        ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_SLOT_ID)
#undef ENGINE_MATERIAL_SLOT_ID
    };
    for (const content::AssetId id : ids) {
      const std::uint32_t *slot = (id != content::kInvalidAssetId)
                                      ? database->textureIndex.find(id)
                                      : nullptr;
      if (slot != nullptr) {
        named[*slot] = true;
      }
    }
  }

  std::size_t freed = 0U;
  for (std::size_t slot = 0U; slot < database->textureAssets.size(); ++slot) {
    if (!database->textureOccupied[slot] || named[slot]) {
      continue;
    }
    const content::AssetId id = database->textureAssets[slot].id;
    const TextureHandle handle = database->textureAssets[slot].runtimeTexture;
    if ((handle != kInvalidTextureHandle) && (releaseFn != nullptr)) {
      releaseFn(handle, userData);
    }
    // Cannot fail: the record is occupied and holds no handle once unloaded.
    static_cast<void>(set_texture_asset_state(
        database, id, content::AssetState::Unloaded, kInvalidTextureHandle));
    static_cast<void>(unregister_texture_asset(database, id));
    ++freed;
  }

  if (freed != 0U) {
    for (std::size_t i = 0U; i < database->materialAssets.size(); ++i) {
      database->materialAssets[i].unregisterableTextureSlots = 0U;
    }
  }
  return freed;
}

} // namespace engine::renderer
