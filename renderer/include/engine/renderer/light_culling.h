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

} // namespace engine::renderer
