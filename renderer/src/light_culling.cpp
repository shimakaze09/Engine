#include "engine/renderer/light_culling.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::renderer {

namespace {

struct Vec4 final {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float w = 0.0F;
};

struct Frustum final {
  Vec4 planes[6]; // left, right, bottom, top, near, far
};

// Multiply two 4x4 column-major matrices: out = a * b.
void mat4_mul(const float *a, const float *b, float *out) noexcept {
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0F;
      for (int k = 0; k < 4; ++k) {
        sum += a[row + k * 4] * b[k + col * 4];
      }
      out[row + col * 4] = sum;
    }
  }
}

// Extract frustum planes from a view-projection matrix (column-major).
// Planes are in world space, pointing inward, normalized.
void extract_frustum_planes(const float *vp, Frustum &f) noexcept {
  // Left: row3 + row0
  f.planes[0] = {vp[3] + vp[0], vp[7] + vp[4], vp[11] + vp[8], vp[15] + vp[12]};
  // Right: row3 - row0
  f.planes[1] = {vp[3] - vp[0], vp[7] - vp[4], vp[11] - vp[8], vp[15] - vp[12]};
  // Bottom: row3 + row1
  f.planes[2] = {vp[3] + vp[1], vp[7] + vp[5], vp[11] + vp[9], vp[15] + vp[13]};
  // Top: row3 - row1
  f.planes[3] = {vp[3] - vp[1], vp[7] - vp[5], vp[11] - vp[9], vp[15] - vp[13]};
  // Near: row3 + row2
  f.planes[4] = {vp[3] + vp[2], vp[7] + vp[6], vp[11] + vp[10],
                 vp[15] + vp[14]};
  // Far: row3 - row2
  f.planes[5] = {vp[3] - vp[2], vp[7] - vp[6], vp[11] - vp[10],
                 vp[15] - vp[14]};

  for (auto &p : f.planes) {
    const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (len > 1e-6F) {
      const float inv = 1.0F / len;
      p.x *= inv;
      p.y *= inv;
      p.z *= inv;
      p.w *= inv;
    }
  }
}

// Test sphere (center, radius) against frustum.
bool sphere_in_frustum(const Frustum &f, float cx, float cy, float cz,
                       float radius) noexcept {
  for (const auto &p : f.planes) {
    const float dist = p.x * cx + p.y * cy + p.z * cz + p.w;
    if (dist < -radius) {
      return false;
    }
  }
  return true;
}

// Build a tile frustum from pixel bounds [x0,x1) x [y0,y1) at screen size
// (w,h) given the view and projection matrices.
void build_tile_vp(int x0, int y0, int x1, int y1, int screenW, int screenH,
                   const float *viewMatrix, const float *projMatrix,
                   float *tileVP) noexcept {
  // Compute NDC bounds.
  const float ndcLeft =
      2.0F * static_cast<float>(x0) / static_cast<float>(screenW) - 1.0F;
  const float ndcRight =
      2.0F * static_cast<float>(x1) / static_cast<float>(screenW) - 1.0F;
  const float ndcBottom =
      2.0F * static_cast<float>(y0) / static_cast<float>(screenH) - 1.0F;
  const float ndcTop =
      2.0F * static_cast<float>(y1) / static_cast<float>(screenH) - 1.0F;

  // Scale + translate to map full NDC [-1,1] to tile's NDC sub-range.
  const float scaleX = 2.0F / (ndcRight - ndcLeft);
  const float scaleY = 2.0F / (ndcTop - ndcBottom);
  const float offsetX = -(ndcRight + ndcLeft) / (ndcRight - ndcLeft);
  const float offsetY = -(ndcTop + ndcBottom) / (ndcTop - ndcBottom);

  // Tile projection = tileClip * proj
  // tileClip scales and offsets x,y in clip space.
  float tileClip[16];
  std::memset(tileClip, 0, sizeof(tileClip));
  tileClip[0] = scaleX;
  tileClip[5] = scaleY;
  tileClip[10] = 1.0F;
  tileClip[12] = offsetX;
  tileClip[13] = offsetY;
  tileClip[15] = 1.0F;

  float tileProj[16];
  mat4_mul(tileClip, projMatrix, tileProj);
  mat4_mul(tileProj, viewMatrix, tileVP);
}

} // namespace

std::size_t compute_tile_buffer_size(int screenW, int screenH) noexcept {
  if (screenW <= 0 || screenH <= 0) {
    return 0;
  }
  const int tileCountX = (screenW + kTileSize - 1) / kTileSize;
  const int tileCountY = (screenH + kTileSize - 1) / kTileSize;
  return static_cast<std::size_t>(tileCountX) *
         static_cast<std::size_t>(tileCountY) *
         static_cast<std::size_t>(kTileDataWidth);
}

bool cull_lights_tiled(const SceneLightData &lightData, const float *viewMatrix,
                       const float *projMatrix, int screenW, int screenH,
                       TileLightData &outData) noexcept {
  if ((viewMatrix == nullptr) || (projMatrix == nullptr) || (screenW <= 0) ||
      (screenH <= 0)) {
    return false;
  }

  const int tileCountX = (screenW + kTileSize - 1) / kTileSize;
  const int tileCountY = (screenH + kTileSize - 1) / kTileSize;
  const int totalTiles = tileCountX * tileCountY;

  outData.tileCountX = tileCountX;
  outData.tileCountY = tileCountY;
  outData.totalTiles = totalTiles;

  const std::size_t requiredSize = static_cast<std::size_t>(totalTiles) *
                                   static_cast<std::size_t>(kTileDataWidth);

  if ((outData.data == nullptr) || (outData.dataSize < requiredSize)) {
    return false;
  }

  std::memset(outData.data, 0, requiredSize * sizeof(float));

  const int pointCount = std::min(static_cast<int>(lightData.pointLightCount),
                                  static_cast<int>(kMaxPointLights));
  const int spotCount = std::min(static_cast<int>(lightData.spotLightCount),
                                 static_cast<int>(kMaxSpotLights));

  for (int ty = 0; ty < tileCountY; ++ty) {
    for (int tx = 0; tx < tileCountX; ++tx) {
      const int tileIdx = ty * tileCountX + tx;
      float *tileRow =
          outData.data + static_cast<std::ptrdiff_t>(tileIdx) * kTileDataWidth;

      const int px0 = tx * kTileSize;
      const int py0 = ty * kTileSize;
      const int px1 = std::min(px0 + kTileSize, screenW);
      const int py1 = std::min(py0 + kTileSize, screenH);

      float tileVP[16];
      build_tile_vp(px0, py0, px1, py1, screenW, screenH, viewMatrix,
                    projMatrix, tileVP);
      Frustum tileFrustum{};
      extract_frustum_planes(tileVP, tileFrustum);

      // Cull point lights.
      int tilePointCount = 0;
      for (int li = 0;
           li < pointCount && tilePointCount < kMaxPointLightsPerTile; ++li) {
        const auto &pl = lightData.pointLights[li];
        if (sphere_in_frustum(tileFrustum, pl.position.x, pl.position.y,
                              pl.position.z, pl.radius)) {
          tileRow[1 + tilePointCount] = static_cast<float>(li);
          ++tilePointCount;
        }
      }
      tileRow[0] = static_cast<float>(tilePointCount);

      // Cull spot lights (stored after point section).
      const int spotBase = 1 + kMaxPointLightsPerTile;
      int tileSpotCount = 0;
      for (int li = 0; li < spotCount && tileSpotCount < kMaxSpotLightsPerTile;
           ++li) {
        const auto &sl = lightData.spotLights[li];
        if (sphere_in_frustum(tileFrustum, sl.position.x, sl.position.y,
                              sl.position.z, sl.radius)) {
          tileRow[spotBase + 1 + tileSpotCount] = static_cast<float>(li);
          ++tileSpotCount;
        }
      }
      tileRow[spotBase] = static_cast<float>(tileSpotCount);
    }
  }

  return true;
}

} // namespace engine::renderer
