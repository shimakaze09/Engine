#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace engine::renderer {

// Forward-declare asset ID type (canonical definition in asset_database.h).
using AssetId = std::uint64_t;
inline constexpr AssetId kInvalidAssetId = 0ULL;

enum class AssetTypeTag : std::uint8_t {
  Unknown = 0,
  Mesh,
  Texture,
  Script,
  Audio,
  Prefab,
  Shader,
  Material
};

struct MeshImportSettings final {
  float scaleFactor = 1.0F;
  std::uint8_t upAxis = 1U; // 0=X, 1=Y, 2=Z
  bool generateNormals = false;
};

struct TextureImportSettings final {
  std::uint8_t format = 0U; // 0=auto
  bool generateMips = true;
  bool sRGB = true;
};

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

  MeshImportSettings meshSettings{};
  TextureImportSettings textureSettings{};
};

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

inline bool asset_metadata_add_tag(AssetMetadata *metadata,
                                   const char *tag) noexcept {
  if ((metadata == nullptr) || (tag == nullptr) ||
      (metadata->tagCount >= AssetMetadata::kMaxTags)) {
    return false;
  }
  if (asset_metadata_has_tag(metadata, tag)) {
    return true;
  }
  const std::size_t len = std::strlen(tag);
  const std::size_t copyLen = (len >= AssetMetadata::kMaxTagLength)
                                  ? (AssetMetadata::kMaxTagLength - 1U)
                                  : len;
  auto &dest = metadata->tags[metadata->tagCount];
  dest.fill('\0');
  std::memcpy(dest.data(), tag, copyLen);
  dest[copyLen] = '\0';
  ++metadata->tagCount;
  return true;
}

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

inline bool asset_metadata_add_dependency(AssetMetadata *metadata,
                                          AssetId depId) noexcept {
  if ((metadata == nullptr) || (depId == kInvalidAssetId) ||
      (metadata->dependencyCount >= AssetMetadata::kMaxDependencies)) {
    return false;
  }
  // Check for duplicates.
  for (std::size_t i = 0U; i < metadata->dependencyCount; ++i) {
    if (metadata->dependencies[i] == depId) {
      return true;
    }
  }
  metadata->dependencies[metadata->dependencyCount] = depId;
  ++metadata->dependencyCount;
  return true;
}

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

} // namespace engine::renderer
