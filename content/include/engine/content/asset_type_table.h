// The one table of asset types the engine knows. Every row names the tag,
// its display label, how an asset of that type reaches the runtime, what
// opening one does, and the path suffixes of its source and cooked forms.
// The tag enum, the descriptors, the path classifier, the editor's filter
// row and the packer's dispatch all expand from it, so a new type is one
// row here and nothing else.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::content {

/// How an asset of a type reaches the runtime.
enum class AssetSourcePolicy : std::uint8_t {
  /// The authored file is loaded as it is.
  Source,
  /// The authored source is cooked by the packer into the runtime form.
  Cooked,
  /// Produced by another asset's cook; never authored on its own.
  Derived,
};

/// What opening an asset of a type means to an author.
enum class AssetPrimaryAction : std::uint8_t {
  /// Nothing beyond selecting it.
  Select,
  /// Place an instance in the scene.
  Instantiate,
  /// Open it as the working document.
  OpenDocument,
  /// Edit its fields in place.
  EditInPlace,
};

// Row: X(Tag, label, policy, action, sourceSuffixes, cookedSuffixes).
// Suffix lists are parenthesised, lower-case, and matched at the end of a
// path without regard to case; () when that form has no suffix of its own.
// A suffix names the kind and never the serialization format, matching the
// cooked side (".mesh", ".anim", ".hull"): an authored document that
// happens to hold JSON is a ".scene", not a ".scene.json", so switching a
// kind's format later costs nothing outside its reader and writer. Row
// order is the bit order of the editor's persisted type filter, so rows
// are only ever appended.
#define ENGINE_ASSET_TYPE_TABLE(X)                                             \
  X(Mesh, "Mesh", Cooked, Instantiate, (".gltf", ".glb"), (".mesh"))           \
  X(Texture, "Texture", Source, Select,                                        \
    (".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx2"), ())                    \
  X(Material, "Material", Source, EditInPlace, (".mat"), ())                   \
  X(Script, "Script", Source, Select, (".lua"), ())                            \
  X(Scene, "Scene", Source, OpenDocument, (".scene"), ())                      \
  X(Animation, "Animation", Derived, Select, (), (".anim", ".skel"))           \
  X(AnimationController, "Anim Controller", Source, Select, (".animctrl"),     \
    ())                                                                        \
  X(Audio, "Sound", Source, Select, (".wav", ".ogg", ".mp3"), ())              \
  X(Unknown, "Other", Source, Select, (), ())                                  \
  X(Prefab, "Prefab", Source, Select, (".prefab"), ())                         \
  X(Shader, "Shader", Cooked, Select, (".sc"), ())

/// Tags every asset record, index entry and query carries.
enum class AssetTypeTag : std::uint8_t {
#define ENGINE_ASSET_TYPE_ENUM(Tag, label, policy, action, sources, cooked)   \
  Tag,
  ENGINE_ASSET_TYPE_TABLE(ENGINE_ASSET_TYPE_ENUM)
#undef ENGINE_ASSET_TYPE_ENUM
};

/// Number of asset types (one per table row).
inline constexpr std::size_t kAssetTypeCount = 0U
#define ENGINE_ASSET_TYPE_COUNT(Tag, label, policy, action, sources, cooked)  \
  +1U
    ENGINE_ASSET_TYPE_TABLE(ENGINE_ASSET_TYPE_COUNT)
#undef ENGINE_ASSET_TYPE_COUNT
    ;

/// One table row in data form.
struct AssetTypeDescriptor final {
  AssetTypeTag tag = AssetTypeTag::Unknown;
  const char *label = "";
  AssetSourcePolicy policy = AssetSourcePolicy::Source;
  AssetPrimaryAction action = AssetPrimaryAction::Select;
  const char *const *sourceSuffixes = nullptr;
  std::size_t sourceSuffixCount = 0U;
  const char *const *cookedSuffixes = nullptr;
  std::size_t cookedSuffixCount = 0U;
};

/// The row for a tag; the Unknown row for a value outside the table.
const AssetTypeDescriptor &asset_type_descriptor(AssetTypeTag tag) noexcept;

/// Display label of a tag ("Mesh", "Anim Controller", ...).
const char *asset_type_label(AssetTypeTag tag) noexcept;

/// What a path's suffix says about it.
struct AssetClassification final {
  AssetTypeTag tag = AssetTypeTag::Unknown;
  /// True when the suffix is the type's authored source form rather than
  /// its cooked or derived form.
  bool source = false;
};

/// Classifies a path by the longest table suffix it ends with, ignoring
/// case; Unknown when no row's suffix matches or the path is null. The
/// name is the whole input: nothing here opens a file.
AssetClassification classify_asset_path(const char *path) noexcept;

} // namespace engine::content
