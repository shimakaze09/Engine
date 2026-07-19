// Declares light culling types and APIs for the Engine renderer system.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/renderer/command_buffer.h"

namespace engine::renderer {

/// Tile size in pixels for tiled light culling.
constexpr int kTileSize = 16;

/// Maximum lights per tile (point + spot separately tracked).
constexpr int kMaxPointLightsPerTile = 32;
constexpr int kMaxSpotLightsPerTile = 16;

/// Tile data layout: [pointCount, pointIdx0..N, spotCount, spotIdx0..N]
/// Width = 1 + kMaxPointLightsPerTile + 1 + kMaxSpotLightsPerTile
constexpr int kTileDataWidth =
    1 + kMaxPointLightsPerTile + 1 + kMaxSpotLightsPerTile;

/// CPU-side tile light culling result.
struct TileLightData final {
  int tileCountX = 0;
  int tileCountY = 0;
  int totalTiles = 0;

  /// Flat array: totalTiles * kTileDataWidth floats.
  /// Each tile row: [pointCount, pointIndices..., spotCount, spotIndices...]
  float *data = nullptr;
  std::size_t dataSize = 0;
};

/// Compute tiled light culling on the CPU.
/// Divides the screen into kTileSize×kTileSize tiles, frustum-tests each
/// point/spot light against each tile, and fills the tile data texture.
///
/// @param lightData  Scene light data (point + spot light arrays).
/// @param viewMatrix View matrix (camera).
/// @param projMatrix Projection matrix.
/// @param screenW    Screen width in pixels.
/// @param screenH    Screen height in pixels.
/// @param outData    Output tile data (caller provides buffer).
/// @return true on success.
bool cull_lights_tiled(const SceneLightData &lightData, const float *viewMatrix,
                       const float *projMatrix, int screenW, int screenH,
                       TileLightData &outData) noexcept;

/// Compute the required buffer size for tile data.
std::size_t compute_tile_buffer_size(int screenW, int screenH) noexcept;

/// Per-light data texture layout (R32F, one row per light) consumed by the
/// deferred lighting shader; lights are fetched by the indices stored in the
/// tile data, keeping the shader free of per-light uniform arrays.
/// Rows [0, kMaxPointLights): point light [posXYZ, colorRGB, intensity,
/// radius]. Rows [kLightDataSpotRow, kLightDataTexHeight): spot light
/// [posXYZ, dirXYZ, colorRGB, intensity, radius, innerCone, outerCone].
constexpr int kLightDataTexWidth = 13;
constexpr int kLightDataSpotRow = static_cast<int>(kMaxPointLights);
constexpr int kLightDataTexHeight =
    kLightDataSpotRow + static_cast<int>(kMaxSpotLights);
constexpr std::size_t kLightDataBufferSize =
    static_cast<std::size_t>(kLightDataTexWidth) *
    static_cast<std::size_t>(kLightDataTexHeight);

/// Packs scene lights into the per-light data texture layout above.
/// Unused rows and trailing floats are zero-filled.
/// @return false if the output buffer is null or too small.
bool pack_light_data(const SceneLightData &lights, float *out,
                     std::size_t outSize) noexcept;

} // namespace engine::renderer
