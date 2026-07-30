// Declares mesh asset types and APIs for the Engine core engine.

#pragma once

#include <cstdint>

namespace engine::core {

inline constexpr std::uint32_t kMeshAssetMagic = 0x4D455348U;
inline constexpr std::uint32_t kMeshAssetVersion = 1U;
inline constexpr std::uint32_t kMeshAssetVersion2 = 2U;
inline constexpr std::uint32_t kMeshAssetVersion3 = 3U;

// v1: 6 floats per vertex (position3 + normal3)
// v2: 8 floats per vertex (position3 + normal3 + uv2)
// v3: 16 floats per vertex (position3 + normal3 + uv2 + joints4 + weights4);
//     skinned meshes always carry the uv slot (zeroed when the source has
//     none) and store joint indices as exact small floats.
struct MeshAssetHeader final {
  std::uint32_t magic = kMeshAssetMagic;
  std::uint32_t version = kMeshAssetVersion;
  std::uint32_t vertexCount = 0U;
  std::uint32_t indexCount = 0U;
};

} // namespace engine::core