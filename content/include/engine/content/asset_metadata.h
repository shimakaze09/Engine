// Declares asset metadata types and APIs for the Engine content system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/content/asset_identity.h"
#include "engine/content/asset_type_table.h"
#include "engine/core/string_util.h"

namespace engine::content {

// Canonical 64-bit asset identity.
using AssetId = std::uint64_t;
inline constexpr AssetId kInvalidAssetId = 0ULL;

/// Generic per-asset load lifecycle shared by every asset class.
enum class AssetState : std::uint8_t { Unloaded, Loading, Ready, Failed };

/// 64-bit FNV-1a id from the canonicalized path (separators normalized to
/// '/'); the one identity constructor shared by runtime and tools.
AssetId make_asset_id_from_path(const char *path) noexcept;

/// 64-bit content-hash id from the file bytes. When the file cannot be
/// opened or a read error would leave a partial hash, falls back to the
/// canonicalized path hash with a logged warning.
AssetId make_asset_id_from_file(const char *path) noexcept;

/// How a mesh source is turned into a cooked mesh: which primitive of
/// the source to take, and how to orient and scale it. Authored data —
/// it lives in the source's ".meta" sidecar, it is what a human edits in
/// the Inspector, and it is part of the cook key, so changing any field
/// recooks the asset.
///
/// This is the one definition. The packer had its own copy with the two
/// sub-asset selectors and this one had neither, which meant the type a
/// caller reached for decided whether "import settings" could name a
/// primitive at all.
struct MeshImportSettings final {
  /// Which mesh of a source that holds several.
  std::int32_t meshIndex = 0;
  /// Which primitive of that mesh.
  std::int32_t primitiveIndex = 0;
  float scaleFactor = 1.0F;
  std::int32_t upAxis = 1; // 0=X, 1=Y, 2=Z
  bool generateNormals = false;

  friend constexpr bool operator==(const MeshImportSettings &,
                                   const MeshImportSettings &) = default;
};

/// Which colour space a texture's texels are read in. Auto leaves it to
/// the material slot that samples the texture, as before textures had
/// settings: base colour and emissive maps sRGB, every other map linear.
/// Srgb and Linear say what the file holds whatever samples it, as
/// Unity's sRGB (Color Texture) checkbox does.
enum class TextureColorSpaceSetting : std::uint8_t { Auto, Srgb, Linear };

/// How a texture is filtered when sampled: Linear blends neighbouring
/// texels (and mips, when it has them); Nearest takes the closest texel,
/// which pixel art wants.
enum class TextureFilterSetting : std::uint8_t { Linear, Nearest };

/// What a texture coordinate outside 0..1 reads: Repeat tiles the image,
/// Clamp holds its edge texels.
enum class TextureWrapSetting : std::uint8_t { Repeat, Clamp };

/// How a texture source is loaded: authored in its ".meta" sidecar, edited
/// in the Inspector. Textures are read from their source at load, so the
/// loader applies these directly; the defaults are what every texture got
/// before it had settings.
struct TextureImportSettings final {
  TextureColorSpaceSetting colorSpace = TextureColorSpaceSetting::Auto;
  /// A full mip chain is generated from the image, so a texture seen small
  /// is not aliased.
  bool generateMips = true;
  TextureFilterSetting filter = TextureFilterSetting::Linear;
  TextureWrapSetting wrap = TextureWrapSetting::Repeat;

  friend constexpr bool operator==(const TextureImportSettings &,
                                   const TextureImportSettings &) = default;
};

/// How a sound source is decoded when it loads: authored in its ".meta"
/// sidecar, edited in the Inspector, as Unity's AudioImporter Sample Rate
/// Setting and Force To Mono. The defaults are what every sound got before
/// it had settings: the file's own rate, and its own channels (down to
/// stereo).
struct AudioImportSettings final {
  /// The rate the sound is resampled to as it decodes, in Hz; 0 keeps the
  /// file's. A lower rate halves the memory a long sound holds.
  std::uint32_t sampleRate = 0U;
  /// Decodes the sound to one channel, so a stereo file spatializes as a
  /// point and takes half the memory.
  bool forceMono = false;

  friend constexpr bool operator==(const AudioImportSettings &,
                                   const AudioImportSettings &) = default;
};

/// The lowest and highest rate a sound's settings may ask for, in Hz.
inline constexpr std::uint32_t kMinAudioImportSampleRate = 8000U;
inline constexpr std::uint32_t kMaxAudioImportSampleRate = 192000U;

/// Stores asset metadata used by the engine.
struct AssetMetadata final {
  static constexpr std::size_t kMaxTags = 16U;
  static constexpr std::size_t kMaxTagLength = 32U;
  static constexpr std::size_t kMaxDependencies = 32U;

  AssetId assetId = kInvalidAssetId;
  /// The asset's persistent identity, resolved from the authored
  /// sidecars: a source's own, or — for a cooked output, which owns no
  /// identity of its own — the producing source's GUID plus the local id
  /// that names this output among that source's several. Nil for an
  /// asset whose source has not been imported yet; `assetId` still
  /// locates it by path in the meantime.
  AssetRef ref{};
  AssetTypeTag typeTag = AssetTypeTag::Unknown;
  std::array<char, 260U> filePath{};
  std::uint64_t fileSize = 0ULL;
  std::int64_t lastModified = 0;
  std::uint64_t checksum = 0ULL;

  std::array<std::array<char, kMaxTagLength>, kMaxTags> tags{};
  std::size_t tagCount = 0U;

  std::array<AssetId, kMaxDependencies> dependencies{};
  std::size_t dependencyCount = 0U;
};

/// True when the metadata carries the tag.
inline bool asset_metadata_has_tag(const AssetMetadata *metadata,
                                   const char *tag) noexcept {
  if ((metadata == nullptr) || (tag == nullptr)) {
    return false;
  }
  for (std::size_t i = 0U; i < metadata->tagCount; ++i) {
    if (std::strcmp(metadata->tags[i].data(), tag) == 0) {
      return true;
    }
  }
  return false;
}

/// Adds a tag; false when full, args are invalid, or the tag text exceeds
/// kMaxTagLength-1 characters (a truncated tag would silently alias
/// queries).
inline bool asset_metadata_add_tag(AssetMetadata *metadata,
                                   const char *tag) noexcept {
  if ((metadata == nullptr) || (tag == nullptr) ||
      (metadata->tagCount >= AssetMetadata::kMaxTags)) {
    return false;
  }
  const std::size_t len = std::strlen(tag);
  if (len >= AssetMetadata::kMaxTagLength) {
    return false;
  }
  if (asset_metadata_has_tag(metadata, tag)) {
    return true;
  }
  auto &dest = metadata->tags[metadata->tagCount];
  dest.fill('\0');
  std::memcpy(dest.data(), tag, len);
  dest[len] = '\0';
  ++metadata->tagCount;
  return true;
}

/// Removes a tag; false when absent or args are invalid. Keeps the order
/// of the rest.
inline bool asset_metadata_remove_tag(AssetMetadata *metadata,
                                      const char *tag) noexcept {
  if ((metadata == nullptr) || (tag == nullptr)) {
    return false;
  }
  for (std::size_t i = 0U; i < metadata->tagCount; ++i) {
    if (std::strcmp(metadata->tags[i].data(), tag) == 0) {
      for (std::size_t j = i + 1U; j < metadata->tagCount; ++j) {
        metadata->tags[j - 1U] = metadata->tags[j];
      }
      --metadata->tagCount;
      metadata->tags[metadata->tagCount].fill('\0');
      return true;
    }
  }
  return false;
}

/// True when `text` can be an asset label (a tag an author gives an asset,
/// as Unity's Asset Labels are): a core name token of at most
/// kMaxTagLength - 1 characters. Labels compare ignoring ASCII case
/// (core::equals_ignoring_case), so "Hero" and "hero" cannot both be given.
inline bool asset_label_is_valid(const char *text) noexcept {
  return core::name_token_is_valid(text, AssetMetadata::kMaxTagLength - 1U);
}

/// An asset's labels, as its sidecar stores them: at most kMaxTags, each
/// valid by asset_label_is_valid and distinct by core::equals_ignoring_case.
struct AssetLabels final {
  std::array<std::array<char, AssetMetadata::kMaxTagLength>,
             AssetMetadata::kMaxTags>
      names{};
  std::size_t count = 0U;

  friend bool operator==(const AssetLabels &a, const AssetLabels &b) noexcept {
    if (a.count != b.count) {
      return false;
    }
    for (std::size_t i = 0U; i < a.count; ++i) {
      if (std::strcmp(a.names[i].data(), b.names[i].data()) != 0) {
        return false;
      }
    }
    return true;
  }
};

/// True when `labels` holds `label`, compared ignoring ASCII case.
inline bool asset_labels_has(const AssetLabels &labels,
                             const char *label) noexcept {
  for (std::size_t i = 0U; i < labels.count; ++i) {
    if (core::equals_ignoring_case(labels.names[i].data(), label)) {
      return true;
    }
  }
  return false;
}

/// Adds `label` at the end. True when it is added or already there; false,
/// with `labels` unchanged, when it is not a valid label or the list is
/// full. Never truncates: a shortened label would be a different one.
inline bool asset_labels_add(AssetLabels *labels, const char *label) noexcept {
  if ((labels == nullptr) || !asset_label_is_valid(label)) {
    return false;
  }
  if (asset_labels_has(*labels, label)) {
    return true;
  }
  if (labels->count >= AssetMetadata::kMaxTags) {
    return false;
  }
  auto &dest = labels->names[labels->count];
  dest.fill('\0');
  std::memcpy(dest.data(), label, std::strlen(label));
  ++labels->count;
  return true;
}

/// Removes `label`, keeping the order of the rest; false when absent.
inline bool asset_labels_remove(AssetLabels *labels,
                                const char *label) noexcept {
  if (labels == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < labels->count; ++i) {
    if (core::equals_ignoring_case(labels->names[i].data(), label)) {
      for (std::size_t j = i + 1U; j < labels->count; ++j) {
        labels->names[j - 1U] = labels->names[j];
      }
      --labels->count;
      labels->names[labels->count].fill('\0');
      return true;
    }
  }
  return false;
}

/// Writes metadata path data.
inline void write_metadata_path(std::array<char, 260U> *outPath,
                                const char *path) noexcept {
  if (outPath == nullptr) {
    return;
  }
  outPath->fill('\0');
  if (path == nullptr) {
    return;
  }
  const std::size_t maxCopy = outPath->size() - 1U;
  const std::size_t srcLen = std::strlen(path);
  const std::size_t copyLen = (srcLen > maxCopy) ? maxCopy : srcLen;
  if (copyLen > 0U) {
    std::memcpy(outPath->data(), path, copyLen);
  }
  (*outPath)[copyLen] = '\0';
}

/// Records a dependency id; false when full or arguments are invalid.
inline bool asset_metadata_add_dependency(AssetMetadata *metadata,
                                          AssetId depId) noexcept {
  if ((metadata == nullptr) || (depId == kInvalidAssetId) ||
      (metadata->dependencyCount >= AssetMetadata::kMaxDependencies)) {
    return false;
  }
  for (std::size_t i = 0U; i < metadata->dependencyCount; ++i) {
    if (metadata->dependencies[i] == depId) {
      return true;
    }
  }
  metadata->dependencies[metadata->dependencyCount] = depId;
  ++metadata->dependencyCount;
  return true;
}

/// True when depId is recorded as a dependency.
inline bool asset_metadata_has_dependency(const AssetMetadata *metadata,
                                          AssetId depId) noexcept {
  if ((metadata == nullptr) || (depId == kInvalidAssetId)) {
    return false;
  }
  for (std::size_t i = 0U; i < metadata->dependencyCount; ++i) {
    if (metadata->dependencies[i] == depId) {
      return true;
    }
  }
  return false;
}

} // namespace engine::content
