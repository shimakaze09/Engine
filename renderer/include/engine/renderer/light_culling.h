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
  /// Each tile entry: [pointCount, pointIndices..., spotCount,
  /// spotIndices...], tiles in row-major screen order. The GPU texture
  /// uploads this same buffer as the 2-D table TileTextureLayout
  /// describes; the flat indexing (tileIdx * kTileDataWidth) reinterprets
  /// as that shape exactly, whatever the row length.
  float *data = nullptr;
  std::size_t dataSize = 0;
};

/// Shape of the GPU tile table: the flat tile array cut into texture rows
/// of tilesPerRow tiles. The row length is independent of the screen's
/// tile columns, because a table that kept one texture row per screen
/// tile row grows as wide as the drawable — tileCountX * kTileDataWidth
/// texels — and passes a 16384 dimension cap once the drawable is wider
/// than about 5232 px, which 4K at a render scale above 1.36 already is.
struct TileTextureLayout final {
  /// Tiles along one texture row; the deferred shader divides the flat
  /// tile index by this to find the row.
  int tilesPerRow = 0;
  int width = 0;
  int height = 0;
  /// width * height. At least totalTiles * kTileDataWidth, and more when
  /// the last row is partial: the upload buffer must cover the whole
  /// rectangle, so the caller pads it to this many floats.
  std::size_t texelCount = 0;
};

/// Lays the tile table out inside maxTextureDimension on both axes. A
/// grid whose tileCountX already fits keeps one texture row per screen
/// tile row; a wider one wraps onto rows of the longest length that fits.
/// @return false, with outLayout zeroed, when the counts are not
/// positive, the limit cannot hold even one tile across, or the wrapped
/// table would be taller than the limit.
bool compute_tile_texture_layout(int tileCountX, int tileCountY,
                                 int maxTextureDimension,
                                 TileTextureLayout &outLayout) noexcept;

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
/// @return true on success. On failure (null matrices, non-positive or
/// overflow-large screen size, missing/undersized buffer) the tile counts
/// in outData are zeroed so callers cannot consume stale dimensions.
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
/// Spot cone angles are packed as cosines — the deferred shader compares
/// the texels against a cosine, not radians. Unused rows and trailing
/// floats are zero-filled.
/// @return false if the output buffer is null or too small.
bool pack_light_data(const SceneLightData &lights, float *out,
                     std::size_t outSize) noexcept;

} // namespace engine::renderer
