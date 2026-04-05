#pragma once

#include <cstdint>

namespace engine::core {

inline constexpr std::uint32_t kMeshAssetMagic = 0x4D455348U;
inline constexpr std::uint32_t kMeshAssetVersion = 1U;

struct MeshAssetHeader final {
  std::uint32_t magic = kMeshAssetMagic;
  std::uint32_t version = kMeshAssetVersion;
  std::uint32_t vertexCount = 0U;
  std::uint32_t indexCount = 0U;
};

} // namespace engine::core