#pragma once

#include <cstdint>

namespace engine::core {

inline constexpr std::uint32_t kMeshAssetMagic = 0x4D455348U;
inline constexpr std::uint32_t kMeshAssetVersion = 1U;
inline constexpr std::uint32_t kMeshAssetVersion2 = 2U;

// v1: 6 floats per vertex (position3 + normal3)
// v2: 8 floats per vertex (position3 + normal3 + uv2)
struct MeshAssetHeader final {
  std::uint32_t magic = kMeshAssetMagic;
  std::uint32_t version = kMeshAssetVersion;
  std::uint32_t vertexCount = 0U;
  std::uint32_t indexCount = 0U;
};

} // namespace engine::core