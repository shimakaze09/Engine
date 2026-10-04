// Declares which import settings each asset type carries in its ".meta"
// sidecar, one row per row of ENGINE_ASSET_TYPE_TABLE: a type added there
// without a row here fails to compile, so a new type cannot silently have
// no settings decision. A sidecar's "importSettings" block is read as its
// asset type's settings and refused on a type that has none.

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "engine/content/asset_type_table.h"

namespace engine::content {

/// The settings an asset type's sidecar block holds.
enum class ImportSettingsKind : std::uint8_t {
  /// The type has no import settings; a block on it is malformed.
  None,
  /// MeshImportSettings: part of the mesh cook key.
  Mesh,
  /// TextureImportSettings: applied by the texture loader.
  Texture,
  /// AudioImportSettings: applied by the sound decoder.
  Audio,
};

/// The newest version of each kind's block this build reads. A block
/// carries its own optional "version" (absent is 1), so one importer's
/// settings change without bumping the whole sidecar's schema, as Godot's
/// importer_version and Unity's per-importer serializedVersion do.
inline constexpr std::uint32_t kMeshImportSettingsVersion = 1U;
inline constexpr std::uint32_t kTextureImportSettingsVersion = 1U;
inline constexpr std::uint32_t kAudioImportSettingsVersion = 1U;

// Row order is ENGINE_ASSET_TYPE_TABLE's; the asserts below hold it there.
#define ENGINE_ASSET_IMPORT_SETTINGS_TABLE(X)                                  \
  X(Mesh, Mesh)                                                                \
  X(Texture, Texture)                                                          \
  X(Material, None)                                                            \
  X(Script, None)                                                              \
  X(Scene, None)                                                               \
  X(Animation, None)                                                           \
  X(AnimationController, None)                                                 \
  X(Audio, Audio)                                                              \
  X(Unknown, None)                                                             \
  X(Prefab, None)                                                              \
  X(Shader, None)                                                              \
  X(Environment, None)                                                         \
  X(NavMesh, None)

namespace detail {

inline constexpr AssetTypeTag kImportSettingsRowTags[] = {
#define ENGINE_IMPORT_SETTINGS_TAG(Tag, Kind) AssetTypeTag::Tag,
    ENGINE_ASSET_IMPORT_SETTINGS_TABLE(ENGINE_IMPORT_SETTINGS_TAG)
#undef ENGINE_IMPORT_SETTINGS_TAG
};

inline constexpr ImportSettingsKind kImportSettingsRowKinds[] = {
#define ENGINE_IMPORT_SETTINGS_KIND(Tag, Kind) ImportSettingsKind::Kind,
    ENGINE_ASSET_IMPORT_SETTINGS_TABLE(ENGINE_IMPORT_SETTINGS_KIND)
#undef ENGINE_IMPORT_SETTINGS_KIND
};

consteval bool import_settings_rows_follow_type_table() {
  for (std::size_t i = 0U; i < std::size(kImportSettingsRowTags); ++i) {
    if (static_cast<std::size_t>(kImportSettingsRowTags[i]) != i) {
      return false;
    }
  }
  return true;
}

} // namespace detail

static_assert(std::size(detail::kImportSettingsRowTags) == kAssetTypeCount,
              "every asset type needs a row in "
              "ENGINE_ASSET_IMPORT_SETTINGS_TABLE");
static_assert(detail::import_settings_rows_follow_type_table(),
              "ENGINE_ASSET_IMPORT_SETTINGS_TABLE rows must follow "
              "ENGINE_ASSET_TYPE_TABLE's order");

/// The settings `tag`'s sidecar block holds.
constexpr ImportSettingsKind import_settings_kind(AssetTypeTag tag) noexcept {
  const auto index = static_cast<std::size_t>(tag);
  return (index < std::size(detail::kImportSettingsRowKinds))
             ? detail::kImportSettingsRowKinds[index]
             : ImportSettingsKind::None;
}

} // namespace engine::content
