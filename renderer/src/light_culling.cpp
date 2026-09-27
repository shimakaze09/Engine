// Implements light culling behavior for the Engine renderer system.

#include "engine/renderer/light_culling.h"

#include "engine/core/logging.h"
#include "engine/math/frustum.h"
#include "engine/math/mat4.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::renderer {

namespace {

/// Loads a column-major matrix handed over as 16 floats.
math::Mat4 load_matrix(const float *m) noexcept {
  return math::Mat4(math::Vec4(m[0], m[1], m[2], m[3]),
                    math::Vec4(m[4], m[5], m[6], m[7]),
                    math::Vec4(m[8], m[9], m[10], m[11]),
                    math::Vec4(m[12], m[13], m[14], m[15]));
}

/// The frustum behind pixel bounds [x0,x1) x [y0,y1) of a (w,h) screen:
/// the view restricted to the tile's NDC sub-rectangle.
math::Frustum tile_frustum(int x0, int y0, int x1, int y1, int screenW,
                           int screenH, const math::Mat4 &view,
                           const math::Mat4 &projection,
                           bool depthZeroToOne) noexcept {
  const auto ndc = [](int pixel, int size) noexcept {
    return (2.0F * static_cast<float>(pixel) / static_cast<float>(size)) - 1.0F;
  };
  const math::Mat4 tileProjection =
      math::sub_rect_projection(projection, ndc(x0, screenW), ndc(y0, screenH),
                                ndc(x1, screenW), ndc(y1, screenH));
  return math::frustum_from_view_projection(math::mul(tileProjection, view),
                                            depthZeroToOne);
}

} // namespace

std::size_t compute_tile_buffer_size(int screenW, int screenH) noexcept {
  if (screenW <= 0 || screenH <= 0) {
    return 0;
  }
  const std::int64_t tileCountX =
      (static_cast<std::int64_t>(screenW) + kTileSize - 1) / kTileSize;
  const std::int64_t tileCountY =
      (static_cast<std::int64_t>(screenH) + kTileSize - 1) / kTileSize;
  return static_cast<std::size_t>(tileCountX) *
         static_cast<std::size_t>(tileCountY) *
         static_cast<std::size_t>(kTileDataWidth);
}

bool compute_tile_texture_layout(int tileCountX, int tileCountY,
                                 int maxTextureDimension,
                                 TileTextureLayout &outLayout) noexcept {
  outLayout = TileTextureLayout{};
  if ((tileCountX <= 0) || (tileCountY <= 0)) {
    return false;
  }
  const std::int64_t tilesThatFit =
      static_cast<std::int64_t>(maxTextureDimension) / kTileDataWidth;
  if (tilesThatFit < 1) {
    return false;
  }

  // A grid that fits keeps tilesPerRow == tileCountX, which makes the
  // texture row the screen tile row: the common case stays the layout a
  // frame capture is easiest to read, and only an oversized drawable
  // wraps.
  const std::int64_t tilesPerRow =
      (static_cast<std::int64_t>(tileCountX) < tilesThatFit)
          ? static_cast<std::int64_t>(tileCountX)
          : tilesThatFit;
  const std::int64_t totalTiles = static_cast<std::int64_t>(tileCountX) *
                                  static_cast<std::int64_t>(tileCountY);
  const std::int64_t rows = (totalTiles + tilesPerRow - 1) / tilesPerRow;
  if (rows > static_cast<std::int64_t>(maxTextureDimension)) {
    return false;
  }

  outLayout.tilesPerRow = static_cast<int>(tilesPerRow);
  outLayout.width = static_cast<int>(tilesPerRow * kTileDataWidth);
  outLayout.height = static_cast<int>(rows);
  outLayout.texelCount = static_cast<std::size_t>(outLayout.width) *
                         static_cast<std::size_t>(outLayout.height);
  return true;
}

bool cull_lights_tiled(const SceneLightData &lightData, const float *viewMatrix,
                       const float *projMatrix, bool depthZeroToOne,
                       int screenW, int screenH,
                       TileLightData &outData) noexcept {
  outData.tileCountX = 0;
  outData.tileCountY = 0;
  outData.totalTiles = 0;
  if ((viewMatrix == nullptr) || (projMatrix == nullptr) || (screenW <= 0) ||
      (screenH <= 0)) {
    return false;
  }

  const std::int64_t wideCountX =
      (static_cast<std::int64_t>(screenW) + kTileSize - 1) / kTileSize;
  const std::int64_t wideCountY =
      (static_cast<std::int64_t>(screenH) + kTileSize - 1) / kTileSize;
  const std::int64_t wideTotal = wideCountX * wideCountY;
  if (wideTotal > static_cast<std::int64_t>(INT32_MAX)) {
    return false;
  }
  const int tileCountX = static_cast<int>(wideCountX);
  const int tileCountY = static_cast<int>(wideCountY);
  const int totalTiles = static_cast<int>(wideTotal);

  const std::size_t requiredSize = static_cast<std::size_t>(totalTiles) *
                                   static_cast<std::size_t>(kTileDataWidth);

  if ((outData.data == nullptr) || (outData.dataSize < requiredSize)) {
    return false;
  }

  outData.tileCountX = tileCountX;
  outData.tileCountY = tileCountY;
  outData.totalTiles = totalTiles;
  const math::Mat4 view = load_matrix(viewMatrix);
  const math::Mat4 projection = load_matrix(projMatrix);

  std::memset(outData.data, 0, requiredSize * sizeof(float));

  const int pointCount = std::min(static_cast<int>(lightData.pointLightCount),
                                  static_cast<int>(kMaxPointLights));
  const int spotCount = std::min(static_cast<int>(lightData.spotLightCount),
                                 static_cast<int>(kMaxSpotLights));

  // Lights a tile's fixed slots could not hold are dropped per tile; the
  // drop is reported once per run instead of silently dimming the tile.
  std::size_t droppedPairs = 0U;
  for (int ty = 0; ty < tileCountY; ++ty) {
    for (int tx = 0; tx < tileCountX; ++tx) {
      const int tileIdx = ty * tileCountX + tx;
      float *tileRow =
          outData.data + static_cast<std::ptrdiff_t>(tileIdx) * kTileDataWidth;

      const int px0 = tx * kTileSize;
      const int py0 = ty * kTileSize;
      const int px1 = std::min(px0 + kTileSize, screenW);
      const int py1 = std::min(py0 + kTileSize, screenH);

      const math::Frustum tileFrustum =
          tile_frustum(px0, py0, px1, py1, screenW, screenH, view, projection,
                       depthZeroToOne);

      int tilePointCount = 0;
      for (int li = 0; li < pointCount; ++li) {
        const auto &pl = lightData.pointLights[li];
        if (!math::frustum_excludes_sphere(tileFrustum, pl.position,
                                           pl.radius)) {
          if (tilePointCount >= kMaxPointLightsPerTile) {
            ++droppedPairs;
            continue;
          }
          tileRow[1 + tilePointCount] = static_cast<float>(li);
          ++tilePointCount;
        }
      }
      tileRow[0] = static_cast<float>(tilePointCount);

      const int spotBase = 1 + kMaxPointLightsPerTile;
      int tileSpotCount = 0;
      for (int li = 0; li < spotCount; ++li) {
        const auto &sl = lightData.spotLights[li];
        if (!math::frustum_excludes_sphere(tileFrustum, sl.position,
                                           sl.radius)) {
          if (tileSpotCount >= kMaxSpotLightsPerTile) {
            ++droppedPairs;
            continue;
          }
          tileRow[spotBase + 1 + tileSpotCount] = static_cast<float>(li);
          ++tileSpotCount;
        }
      }
      tileRow[spotBase] = static_cast<float>(tileSpotCount);
    }
  }

  if (droppedPairs > 0U) {
    static bool warnedTileOverflow = false;
    if (!warnedTileOverflow) {
      warnedTileOverflow = true;
      char message[160] = {};
      std::snprintf(message, sizeof(message),
                    "tile light cap reached: %zu light/tile pairs dropped "
                    "this frame (%d point, %d spot per tile); further drops "
                    "are not logged",
                    droppedPairs, kMaxPointLightsPerTile,
                    kMaxSpotLightsPerTile);
      core::log_message(core::LogLevel::Warning, "renderer", message);
    }
  }

  return true;
}

bool pack_light_data(const SceneLightData &lights, float *out,
                     std::size_t outSize) noexcept {
  if ((out == nullptr) || (outSize < kLightDataBufferSize)) {
    return false;
  }

  std::memset(out, 0, kLightDataBufferSize * sizeof(float));

  const std::size_t pointCount =
      std::min(lights.pointLightCount, kMaxPointLights);
  for (std::size_t i = 0U; i < pointCount; ++i) {
    const auto &pl = lights.pointLights[i];
    float *row = out + i * static_cast<std::size_t>(kLightDataTexWidth);
    row[0] = pl.position.x;
    row[1] = pl.position.y;
    row[2] = pl.position.z;
    row[3] = pl.color.x;
    row[4] = pl.color.y;
    row[5] = pl.color.z;
    row[6] = pl.intensity;
    row[7] = pl.radius;
  }

  const std::size_t spotCount = std::min(lights.spotLightCount, kMaxSpotLights);
  for (std::size_t i = 0U; i < spotCount; ++i) {
    const auto &sl = lights.spotLights[i];
    float *row = out + (static_cast<std::size_t>(kLightDataSpotRow) + i) *
                           static_cast<std::size_t>(kLightDataTexWidth);
    row[0] = sl.position.x;
    row[1] = sl.position.y;
    row[2] = sl.position.z;
    row[3] = sl.direction.x;
    row[4] = sl.direction.y;
    row[5] = sl.direction.z;
    row[6] = sl.color.x;
    row[7] = sl.color.y;
    row[8] = sl.color.z;
    row[9] = sl.intensity;
    row[10] = sl.radius;
    row[11] = std::cos(sl.innerConeAngle);
    row[12] = std::cos(sl.outerConeAngle);
  }

  return true;
}

} // namespace engine::renderer
