// Declares asset metadata types and APIs for the Engine content system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/content/asset_type_table.h"

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

/// Stores asset metadata used by the engine.
struct AssetMetadata final {
  static constexpr std::size_t kMaxTags = 16U;
  static constexpr std::size_t kMaxTagLength = 32U;
  static constexpr std::size_t kMaxDependencies = 32U;

  AssetId assetId = kInvalidAssetId;
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
