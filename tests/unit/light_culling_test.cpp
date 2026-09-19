// Verifies light culling test behavior for the Engine test suite.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/core/logging.h"
#include "engine/renderer/command_buffer.h"
#include "engine/math/transform.h"
#include "engine/renderer/light_culling.h"

namespace {

// ---------------------------------------------------------------------------
// Test 1: Empty scene — zero lights produce all-zero tile data.
// ---------------------------------------------------------------------------

int verify_empty_scene_culling() {
  engine::renderer::SceneLightData lights{};
  lights.pointLightCount = 0U;
  lights.spotLightCount = 0U;

  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  // Identity view matrix.
  const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // Simple perspective-like projection.
  const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};

  const std::size_t bufSize =
      engine::renderer::compute_tile_buffer_size(kWidth, kHeight);
  if (bufSize == 0U) {
    return 100;
  }

  std::vector<float> buffer(bufSize, -1.0F);

  engine::renderer::TileLightData tiles{};
  tiles.data = buffer.data();
  tiles.dataSize = bufSize;

  if (!engine::renderer::cull_lights_tiled(lights, view, proj, kWidth, kHeight,
                                           tiles)) {
    return 101;
  }

  if (tiles.tileCountX <= 0 || tiles.tileCountY <= 0) {
    return 102;
  }

  // Every point-light count and spot-light count should be zero.
  for (int t = 0; t < tiles.totalTiles; ++t) {
    const std::size_t base =
        static_cast<std::size_t>(t) *
        static_cast<std::size_t>(engine::renderer::kTileDataWidth);
    const float pointCount = buffer[base]; // first float = point count
    const std::size_t spotOffset =
        base + 1U +
        static_cast<std::size_t>(engine::renderer::kMaxPointLightsPerTile);
    const float spotCount = buffer[spotOffset];

    if (pointCount != 0.0F) {
      return 103;
    }
    if (spotCount != 0.0F) {
      return 104;
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 2: Single point light at the origin should hit the center tile.
// ---------------------------------------------------------------------------

int verify_single_point_light_center() {
  engine::renderer::SceneLightData lights{};
  lights.pointLightCount = 1U;
  lights.pointLights[0].position = engine::math::Vec3(0.0F, 0.0F, -5.0F);
  lights.pointLights[0].color = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  lights.pointLights[0].intensity = 1.0F;
  lights.pointLights[0].radius = 50.0F;

  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};

  const std::size_t bufSize =
      engine::renderer::compute_tile_buffer_size(kWidth, kHeight);
  if (bufSize == 0U) {
    return 200;
  }

  std::vector<float> buffer(bufSize, 0.0F);

  engine::renderer::TileLightData tiles{};
  tiles.data = buffer.data();
  tiles.dataSize = bufSize;

  if (!engine::renderer::cull_lights_tiled(lights, view, proj, kWidth, kHeight,
                                           tiles)) {
    return 201;
  }

  // With a large radius the light should appear in at least one tile.
  bool foundInAnyTile = false;
  for (int t = 0; t < tiles.totalTiles; ++t) {
    const std::size_t base =
        static_cast<std::size_t>(t) *
        static_cast<std::size_t>(engine::renderer::kTileDataWidth);
    if (buffer[base] > 0.0F) {
      foundInAnyTile = true;
      break;
    }
  }

  if (!foundInAnyTile) {
    return 202;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Orthographic projection (#221): tiled culling is matrix-derived, so an
// ortho VP must include an in-volume light and exclude one beyond the
// ortho half-width that a perspective frustum of the same pose would keep.
// ---------------------------------------------------------------------------

int verify_ortho_projection_culling() {
  engine::renderer::SceneLightData lights{};
  lights.pointLightCount = 2U;
  lights.pointLights[0].position = engine::math::Vec3(0.0F, 0.0F, -5.0F);
  lights.pointLights[0].intensity = 1.0F;
  lights.pointLights[0].radius = 1.0F;
  lights.pointLights[1].position = engine::math::Vec3(50.0F, 0.0F, -5.0F);
  lights.pointLights[1].intensity = 1.0F;
  lights.pointLights[1].radius = 1.0F;

  constexpr int kWidth = 64;
  constexpr int kHeight = 64;

  const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // math::ortho(-2, 2, -2, 2, 0.1, 100) in column-major float form.
  const engine::math::Mat4 orthoProj =
      engine::math::ortho(-2.0F, 2.0F, -2.0F, 2.0F, 0.1F, 100.0F);
  float proj[16] = {};
  for (int c = 0; c < 4; ++c) {
    proj[c * 4 + 0] = orthoProj.columns[c].x;
    proj[c * 4 + 1] = orthoProj.columns[c].y;
    proj[c * 4 + 2] = orthoProj.columns[c].z;
    proj[c * 4 + 3] = orthoProj.columns[c].w;
  }

  const std::size_t bufSize =
      engine::renderer::compute_tile_buffer_size(kWidth, kHeight);
  if (bufSize == 0U) {
    return 700;
  }
  std::vector<float> buffer(bufSize, 0.0F);
  engine::renderer::TileLightData tiles{};
  tiles.data = buffer.data();
  tiles.dataSize = bufSize;

  if (!engine::renderer::cull_lights_tiled(lights, view, proj, kWidth, kHeight,
                                           tiles)) {
    return 701;
  }

  bool foundInVolume = false;
  bool foundOutOfVolume = false;
  for (int t = 0; t < tiles.totalTiles; ++t) {
    const std::size_t base =
        static_cast<std::size_t>(t) *
        static_cast<std::size_t>(engine::renderer::kTileDataWidth);
    const int count = static_cast<int>(buffer[base]);
    for (int i = 0; i < count; ++i) {
      const int lightIndex = static_cast<int>(buffer[base + 1U +
                                                     static_cast<std::size_t>(i)]);
      if (lightIndex == 0) {
        foundInVolume = true;
      }
      if (lightIndex == 1) {
        foundOutOfVolume = true;
      }
    }
  }
  if (!foundInVolume) {
    return 702; // the centered light must survive ortho culling
  }
  if (foundOutOfVolume) {
    return 703; // x=50 lies far outside the 2-unit ortho half-width
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 3: Tile dimension calculation.
// ---------------------------------------------------------------------------

int verify_tile_dimensions() {
  engine::renderer::SceneLightData lights{};
  lights.pointLightCount = 0U;
  lights.spotLightCount = 0U;

  constexpr int kWidth = 64;
  constexpr int kHeight = 64;
  constexpr int kExpectedTilesX = 4; // 64 / 16
  constexpr int kExpectedTilesY = 4;
  constexpr int kExpectedTotal = kExpectedTilesX * kExpectedTilesY;

  const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};

  const std::size_t bufSize =
      engine::renderer::compute_tile_buffer_size(kWidth, kHeight);
  if (bufSize == 0U) {
    return 300;
  }

  std::vector<float> buffer(bufSize, 0.0F);

  engine::renderer::TileLightData tiles{};
  tiles.data = buffer.data();
  tiles.dataSize = bufSize;

  if (!engine::renderer::cull_lights_tiled(lights, view, proj, kWidth, kHeight,
                                           tiles)) {
    return 301;
  }

  if (tiles.tileCountX != kExpectedTilesX) {
    return 302;
  }
  if (tiles.tileCountY != kExpectedTilesY) {
    return 303;
  }
  if (tiles.totalTiles != kExpectedTotal) {
    return 304;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 4: 256 lights stress test (P1-M5-B4d).
// Verifies tiled culling runs correctly with many lights and that every
// light index assigned to a tile is within bounds.
// ---------------------------------------------------------------------------

int verify_256_lights_stress() {
  engine::renderer::SceneLightData lights{};

  // Fill 128 point lights in a grid.
  for (std::size_t i = 0U; i < 128U; ++i) {
    const float x = static_cast<float>(i % 16U) * 5.0F - 40.0F;
    const float y = static_cast<float>(i / 16U) * 5.0F - 20.0F;
    lights.pointLights[i].position = engine::math::Vec3(x, y, -10.0F);
    lights.pointLights[i].color = engine::math::Vec3(1.0F, 1.0F, 1.0F);
    lights.pointLights[i].intensity = 1.0F;
    lights.pointLights[i].radius = 8.0F;
  }
  lights.pointLightCount = 128U;

  // Fill 64 spot lights.
  for (std::size_t i = 0U; i < 64U; ++i) {
    const float x = static_cast<float>(i % 8U) * 10.0F - 35.0F;
    const float y = static_cast<float>(i / 8U) * 10.0F - 35.0F;
    lights.spotLights[i].position = engine::math::Vec3(x, y, -8.0F);
    lights.spotLights[i].direction = engine::math::Vec3(0.0F, 0.0F, -1.0F);
    lights.spotLights[i].color = engine::math::Vec3(1.0F, 0.5F, 0.0F);
    lights.spotLights[i].intensity = 2.0F;
    lights.spotLights[i].radius = 12.0F;
    lights.spotLights[i].innerConeAngle = 0.3F;
    lights.spotLights[i].outerConeAngle = 0.6F;
  }
  lights.spotLightCount = 64U;

  constexpr int kWidth = 256;
  constexpr int kHeight = 256;

  const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};

  const std::size_t bufSize =
      engine::renderer::compute_tile_buffer_size(kWidth, kHeight);
  if (bufSize == 0U) {
    return 400;
  }

  std::vector<float> buffer(bufSize, 0.0F);

  engine::renderer::TileLightData tiles{};
  tiles.data = buffer.data();
  tiles.dataSize = bufSize;

  if (!engine::renderer::cull_lights_tiled(lights, view, proj, kWidth, kHeight,
                                           tiles)) {
    return 401;
  }

  // 256/16 = 16 tiles per axis.
  if (tiles.tileCountX != 16 || tiles.tileCountY != 16) {
    return 402;
  }
  if (tiles.totalTiles != 256) {
    return 403;
  }

  // Validate every tile's light indices are in bounds.
  for (int t = 0; t < tiles.totalTiles; ++t) {
    const std::size_t base =
        static_cast<std::size_t>(t) *
        static_cast<std::size_t>(engine::renderer::kTileDataWidth);
    const int pc = static_cast<int>(buffer[base]);
    if (pc < 0 || pc > engine::renderer::kMaxPointLightsPerTile) {
      return 404;
    }
    for (int i = 0; i < pc; ++i) {
      const int idx =
          static_cast<int>(buffer[base + 1U + static_cast<std::size_t>(i)]);
      if (idx < 0 || idx >= 128) {
        return 405;
      }
    }
    const std::size_t spotOff =
        base + 1U +
        static_cast<std::size_t>(engine::renderer::kMaxPointLightsPerTile);
    const int sc = static_cast<int>(buffer[spotOff]);
    if (sc < 0 || sc > engine::renderer::kMaxSpotLightsPerTile) {
      return 406;
    }
    for (int i = 0; i < sc; ++i) {
      const int idx =
          static_cast<int>(buffer[spotOff + 1U + static_cast<std::size_t>(i)]);
      if (idx < 0 || idx >= 64) {
        return 407;
      }
    }
  }

  // At least some tiles should have lights.
  bool anyPoint = false;
  bool anySpot = false;
  for (int t = 0; t < tiles.totalTiles; ++t) {
    const std::size_t base =
        static_cast<std::size_t>(t) *
        static_cast<std::size_t>(engine::renderer::kTileDataWidth);
    if (buffer[base] > 0.0F)
      anyPoint = true;
    const std::size_t spotOff =
        base + 1U +
        static_cast<std::size_t>(engine::renderer::kMaxPointLightsPerTile);
    if (buffer[spotOff] > 0.0F)
      anySpot = true;
  }
  if (!anyPoint) {
    return 408;
  }
  if (!anySpot) {
    return 409;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 5: Per-light data texture packing — exact layout the deferred
// lighting shader hardcodes (light_data/light_data3 fetch offsets).
// ---------------------------------------------------------------------------

int verify_pack_light_data() {
  namespace er = engine::renderer;

  // The shader hardcodes these as TILE_MAX_POINT_LIGHTS=32 and
  // LIGHT_DATA_SPOT_ROW=128 with 13-float rows; lock them here.
  static_assert(er::kMaxPointLightsPerTile == 32);
  static_assert(er::kMaxSpotLightsPerTile == 16);
  static_assert(er::kLightDataTexWidth == 13);
  static_assert(er::kLightDataSpotRow == 128);
  static_assert(er::kLightDataTexHeight == 192);

  er::SceneLightData lights{};
  lights.pointLightCount = 2U;
  lights.pointLights[0].position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  lights.pointLights[0].color = engine::math::Vec3(0.5F, 0.25F, 0.125F);
  lights.pointLights[0].intensity = 4.0F;
  lights.pointLights[0].radius = 9.0F;
  lights.pointLights[1].position = engine::math::Vec3(-1.0F, -2.0F, -3.0F);
  lights.pointLights[1].color = engine::math::Vec3(1.0F, 0.0F, 1.0F);
  lights.pointLights[1].intensity = 2.5F;
  lights.pointLights[1].radius = 7.5F;

  lights.spotLightCount = 1U;
  lights.spotLights[0].position = engine::math::Vec3(10.0F, 20.0F, 30.0F);
  lights.spotLights[0].direction = engine::math::Vec3(0.0F, -1.0F, 0.0F);
  lights.spotLights[0].color = engine::math::Vec3(0.75F, 0.5F, 0.25F);
  lights.spotLights[0].intensity = 6.0F;
  lights.spotLights[0].radius = 15.0F;
  lights.spotLights[0].innerConeAngle = 0.25F;
  lights.spotLights[0].outerConeAngle = 0.5F;

  std::vector<float> buffer(er::kLightDataBufferSize, -1.0F);

  // Rejects null/too-small buffers.
  if (er::pack_light_data(lights, nullptr, buffer.size())) {
    return 500;
  }
  if (er::pack_light_data(lights, buffer.data(), buffer.size() - 1U)) {
    return 501;
  }

  if (!er::pack_light_data(lights, buffer.data(), buffer.size())) {
    return 502;
  }

  const auto rowW = static_cast<std::size_t>(er::kLightDataTexWidth);

  // Point light row 0: posXYZ, colorRGB, intensity, radius, zero padding.
  const float expectedPoint0[13] = {1.0F, 2.0F,  3.0F,   0.5F, 0.25F,
                                    0.125F, 4.0F, 9.0F, 0.0F, 0.0F,
                                    0.0F,   0.0F, 0.0F};
  for (std::size_t x = 0U; x < rowW; ++x) {
    if (buffer[x] != expectedPoint0[x]) {
      return 503;
    }
  }

  // Point light row 1.
  const float expectedPoint1[13] = {-1.0F, -2.0F, -3.0F, 1.0F, 0.0F,
                                    1.0F,  2.5F,  7.5F,  0.0F, 0.0F,
                                    0.0F,  0.0F,  0.0F};
  for (std::size_t x = 0U; x < rowW; ++x) {
    if (buffer[rowW + x] != expectedPoint1[x]) {
      return 504;
    }
  }

  // Unused point row 2 is fully zeroed.
  for (std::size_t x = 0U; x < rowW; ++x) {
    if (buffer[2U * rowW + x] != 0.0F) {
      return 505;
    }
  }

  // Spot light row: posXYZ, dirXYZ, colorRGB, intensity, radius, cones.
  // Cone texels hold cosines of the stored radian angles because the shader
  // compares them against dot(L, -spotDir).
  const float expectedSpot0[13] = {10.0F, 20.0F, 30.0F, 0.0F,  -1.0F,
                                   0.0F,  0.75F, 0.5F,  0.25F, 6.0F,
                                   15.0F, std::cos(0.25F), std::cos(0.5F)};
  const std::size_t spotBase =
      static_cast<std::size_t>(er::kLightDataSpotRow) * rowW;
  for (std::size_t x = 0U; x < rowW; ++x) {
    if (buffer[spotBase + x] != expectedSpot0[x]) {
      return 506;
    }
  }

  // Unused spot row and the last row are fully zeroed.
  for (std::size_t x = 0U; x < rowW; ++x) {
    if (buffer[spotBase + rowW + x] != 0.0F) {
      return 507;
    }
    if (buffer[er::kLightDataBufferSize - rowW + x] != 0.0F) {
      return 508;
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 6 (audit M-06): failed culling zeroes the output tile counts so a
// caller can never consume stale dimensions, and tile arithmetic is checked
// instead of overflowing int before validation.
// ---------------------------------------------------------------------------

int verify_cull_failure_zeroes_output() {
  engine::renderer::SceneLightData lights{};
  float identity[16] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};

  std::vector<float> tiny(1U, 0.0F);
  engine::renderer::TileLightData undersized{};
  undersized.totalTiles = 123;
  undersized.tileCountX = 7;
  undersized.tileCountY = 9;
  undersized.data = tiny.data();
  undersized.dataSize = tiny.size();
  if (engine::renderer::cull_lights_tiled(lights, identity, identity, 640, 480,
                                          undersized)) {
    return 600;
  }
  if ((undersized.totalTiles != 0) || (undersized.tileCountX != 0) ||
      (undersized.tileCountY != 0)) {
    return 601;
  }

  engine::renderer::TileLightData nullMats{};
  nullMats.totalTiles = 55;
  if (engine::renderer::cull_lights_tiled(lights, nullptr, nullptr, 640, 480,
                                          nullMats)) {
    return 602;
  }
  if (nullMats.totalTiles != 0) {
    return 603;
  }

  std::vector<float> buffer(1024U, 0.0F);
  engine::renderer::TileLightData huge{};
  huge.totalTiles = 77;
  huge.data = buffer.data();
  huge.dataSize = buffer.size();
  const int hugeDim = 0x7FFFFFF0;
  if (engine::renderer::cull_lights_tiled(lights, identity, identity, hugeDim,
                                          hugeDim, huge)) {
    return 604;
  }
  if (huge.totalTiles != 0) {
    return 605;
  }

  const std::size_t bytesForMax =
      engine::renderer::compute_tile_buffer_size(hugeDim, 1);
  const std::size_t expectedTiles =
      (static_cast<std::size_t>(hugeDim) +
       static_cast<std::size_t>(engine::renderer::kTileSize) - 1U) /
      static_cast<std::size_t>(engine::renderer::kTileSize);
  if (bytesForMax !=
      expectedTiles *
          static_cast<std::size_t>(engine::renderer::kTileDataWidth)) {
    return 606;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 8: Tile table layout — the GPU table stays inside the device's
// texture dimension limit at any drawable size (issue #565 row 5). A table
// as wide as the screen's tile columns passes 16384 texels once the
// drawable is wider than about 5232 px, and the deferred path then loses
// every local light.
// ---------------------------------------------------------------------------

/// Tile columns or rows for a drawable extent, as the culler counts them.
int tiles_for(int pixels) {
  return (pixels + engine::renderer::kTileSize - 1) /
         engine::renderer::kTileSize;
}

/// Walks every tile of a grid through the addressing the deferred shader
/// uses — flat index, whole-number division by tilesPerRow — and checks it
/// lands on the texel the flat CPU buffer holds for that tile, inside the
/// rectangle. Returns false at the first tile that does not.
bool layout_addresses_every_tile(
    int tileCountX, int tileCountY,
    const engine::renderer::TileTextureLayout &layout) {
  const long long width = layout.width;
  for (int y = 0; y < tileCountY; ++y) {
    for (int x = 0; x < tileCountX; ++x) {
      const long long flat = static_cast<long long>(y) * tileCountX + x;
      const long long row = flat / layout.tilesPerRow;
      const long long column =
          (flat - row * layout.tilesPerRow) * engine::renderer::kTileDataWidth;
      if ((row < 0) || (row >= layout.height)) {
        return false;
      }
      if ((column + engine::renderer::kTileDataWidth) > width) {
        return false;
      }
      // The buffer is uploaded unrepacked, so the texel's linear offset
      // has to be the tile's offset in the flat array.
      if ((row * width + column) !=
          (flat * engine::renderer::kTileDataWidth)) {
        return false;
      }
    }
  }
  return true;
}

int verify_tile_texture_layout() {
  using engine::renderer::compute_tile_texture_layout;
  using engine::renderer::kTileDataWidth;
  using engine::renderer::TileTextureLayout;
  constexpr int kDesktopLimit = 16384;

  // A grid that fits keeps one texture row per screen tile row, with no
  // padding: 1080p and native 4K are laid out as they always were.
  {
    const int sizes[2][2] = {{1920, 1080}, {3840, 2160}};
    for (const auto &size : sizes) {
      const int tilesX = tiles_for(size[0]);
      const int tilesY = tiles_for(size[1]);
      TileTextureLayout layout{};
      if (!compute_tile_texture_layout(tilesX, tilesY, kDesktopLimit, layout)) {
        return 800;
      }
      if ((layout.tilesPerRow != tilesX) || (layout.height != tilesY) ||
          (layout.width != tilesX * kTileDataWidth)) {
        return 801;
      }
      if (layout.texelCount != static_cast<std::size_t>(tilesX) *
                                   static_cast<std::size_t>(tilesY) *
                                   static_cast<std::size_t>(kTileDataWidth)) {
        return 802;
      }
    }
  }

  // The exact boundary: 327 tile columns is 16350 texels and fits; 328 is
  // 16400 and must wrap.
  {
    TileTextureLayout fits{};
    if (!compute_tile_texture_layout(327, 100, kDesktopLimit, fits) ||
        (fits.tilesPerRow != 327) || (fits.width != 16350) ||
        (fits.height != 100)) {
      return 810;
    }
    TileTextureLayout wraps{};
    if (!compute_tile_texture_layout(328, 100, kDesktopLimit, wraps)) {
      return 811;
    }
    // 32800 tiles in rows of 327: 100 full rows and one of 100 tiles.
    if ((wraps.tilesPerRow != 327) || (wraps.width != 16350) ||
        (wraps.height != 101)) {
      return 812;
    }
    if (!layout_addresses_every_tile(328, 100, wraps)) {
      return 813;
    }
  }

  // The reported case: 4K at r_render_scale 1.5 is a 5760x3240 drawable,
  // 360x203 tiles. One texture row per screen row would be 18000 texels
  // wide, which is what the device refused.
  {
    const int tilesX = tiles_for(5760);
    const int tilesY = tiles_for(3240);
    if ((tilesX != 360) || (tilesY != 203) ||
        (tilesX * kTileDataWidth <= kDesktopLimit)) {
      return 820;
    }
    TileTextureLayout layout{};
    if (!compute_tile_texture_layout(tilesX, tilesY, kDesktopLimit, layout)) {
      return 821;
    }
    if ((layout.width > kDesktopLimit) || (layout.height > kDesktopLimit)) {
      return 822;
    }
    // 73080 tiles in rows of 327: 223 full rows and one of 159.
    if ((layout.tilesPerRow != 327) || (layout.width != 16350) ||
        (layout.height != 224)) {
      return 823;
    }
    // The last row is partial, so the rectangle is larger than the tile
    // data and the upload buffer has to be padded up to it.
    const std::size_t tileFloats = static_cast<std::size_t>(tilesX) *
                                   static_cast<std::size_t>(tilesY) *
                                   static_cast<std::size_t>(kTileDataWidth);
    if ((layout.texelCount != static_cast<std::size_t>(16350) * 224U) ||
        (layout.texelCount <= tileFloats)) {
      return 824;
    }
    if (!layout_addresses_every_tile(tilesX, tilesY, layout)) {
      return 825;
    }
  }

  // 8K, and a device with a smaller limit than the desktop one: the row
  // length follows the limit it is given, not a constant.
  {
    TileTextureLayout eightK{};
    if (!compute_tile_texture_layout(tiles_for(7680), tiles_for(4320),
                                     kDesktopLimit, eightK) ||
        (eightK.width > kDesktopLimit) || (eightK.height > kDesktopLimit) ||
        !layout_addresses_every_tile(tiles_for(7680), tiles_for(4320),
                                     eightK)) {
      return 830;
    }
    TileTextureLayout small{};
    if (!compute_tile_texture_layout(tiles_for(1920), tiles_for(1080), 4096,
                                     small)) {
      return 831;
    }
    // 4096 / 50 = 81 tiles across; 120x68 = 8160 tiles is 101 rows.
    if ((small.tilesPerRow != 81) || (small.width != 4050) ||
        (small.height != 101) ||
        !layout_addresses_every_tile(tiles_for(1920), tiles_for(1080),
                                     small)) {
      return 832;
    }
  }

  // One tile, and a single row that exactly fills the limit's tile count.
  {
    TileTextureLayout one{};
    if (!compute_tile_texture_layout(1, 1, kDesktopLimit, one) ||
        (one.tilesPerRow != 1) || (one.width != kTileDataWidth) ||
        (one.height != 1) ||
        (one.texelCount != static_cast<std::size_t>(kTileDataWidth))) {
      return 840;
    }
  }

  // Refusals leave the output zeroed so a caller cannot size a texture
  // from a stale layout: no tiles, a limit too small for one tile across,
  // and a table that would wrap past the limit's height.
  {
    const int refused[4][3] = {
        {0, 10, kDesktopLimit},
        {10, 0, kDesktopLimit},
        {10, 10, kTileDataWidth - 1},
        // 50-texel limit: one tile per row, so 10x10 tiles need 100 rows.
        {10, 10, kTileDataWidth},
    };
    for (const auto &entry : refused) {
      TileTextureLayout layout{};
      layout.tilesPerRow = 7;
      layout.width = 7;
      layout.height = 7;
      layout.texelCount = 7U;
      if (compute_tile_texture_layout(entry[0], entry[1], entry[2], layout)) {
        return 850;
      }
      if ((layout.tilesPerRow != 0) || (layout.width != 0) ||
          (layout.height != 0) || (layout.texelCount != 0U)) {
        return 851;
      }
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 9: The per-tile cap — a tile holds 32 point and 16 spot lights
// (issue #565 row 3). Past that a light is dropped from that tile, which
// dims it; on base nothing said so.
// ---------------------------------------------------------------------------

/// Counts the culler's tile-cap report among the log lines of a run.
struct TileCapLog final {
  int reports = 0;
};

void count_tile_cap_reports(engine::core::LogLevel level, const char *channel,
                            const char *message, void *userData) noexcept {
  auto *log = static_cast<TileCapLog *>(userData);
  if ((log != nullptr) && (level == engine::core::LogLevel::Warning) &&
      (channel != nullptr) && (std::strcmp(channel, "renderer") == 0) &&
      (message != nullptr) &&
      (std::strstr(message, "tile light cap reached") != nullptr)) {
    ++log->reports;
  }
}

/// Forty point lights and twenty spot lights, all centred in front of a
/// one-tile view with a radius that covers it: eight point and four spot
/// lights past what the tile holds.
int verify_tile_cap_keeps_the_first_lights() {
  engine::renderer::SceneLightData lights{};
  lights.pointLightCount = 40U;
  lights.spotLightCount = 20U;
  for (std::size_t i = 0U; i < 40U; ++i) {
    lights.pointLights[i].position = engine::math::Vec3(0.0F, 0.0F, -5.0F);
    lights.pointLights[i].radius = 50.0F;
  }
  for (std::size_t i = 0U; i < 20U; ++i) {
    lights.spotLights[i].position = engine::math::Vec3(0.0F, 0.0F, -5.0F);
    lights.spotLights[i].radius = 50.0F;
  }

  constexpr int kSize = engine::renderer::kTileSize; // exactly one tile
  const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};
  std::vector<float> buffer(
      engine::renderer::compute_tile_buffer_size(kSize, kSize), -1.0F);
  engine::renderer::TileLightData tiles{};
  tiles.data = buffer.data();
  tiles.dataSize = buffer.size();

  // Twice: the report is for the run, not for every frame that overflows.
  for (int frame = 0; frame < 2; ++frame) {
    if (!engine::renderer::cull_lights_tiled(lights, view, proj, kSize, kSize,
                                             tiles)) {
      return 900;
    }
  }
  if (tiles.totalTiles != 1) {
    return 901;
  }

  // The tile is full, not overfull, and holds the lowest-indexed lights in
  // order: which lights a crowded tile keeps is a function of the scene.
  const int pointCap = engine::renderer::kMaxPointLightsPerTile;
  const int spotCap = engine::renderer::kMaxSpotLightsPerTile;
  if (static_cast<int>(buffer[0]) != pointCap) {
    return 902;
  }
  for (int i = 0; i < pointCap; ++i) {
    if (static_cast<int>(buffer[static_cast<std::size_t>(1 + i)]) != i) {
      return 903;
    }
  }
  const std::size_t spotBase = static_cast<std::size_t>(1 + pointCap);
  if (static_cast<int>(buffer[spotBase]) != spotCap) {
    return 904;
  }
  for (int i = 0; i < spotCap; ++i) {
    if (static_cast<int>(buffer[spotBase + 1U + static_cast<std::size_t>(i)]) !=
        i) {
      return 905;
    }
  }
  return 0;
}

/// Every case but the tile cap's, in the order they have always run.
int run_cases() {
  int result = verify_empty_scene_culling();
  if (result != 0) {
    return result;
  }

  result = verify_single_point_light_center();
  if (result != 0) {
    return result;
  }

  result = verify_ortho_projection_culling();
  if (result != 0) {
    return result;
  }

  result = verify_tile_dimensions();
  if (result != 0) {
    return result;
  }

  result = verify_256_lights_stress();
  if (result != 0) {
    return result;
  }

  result = verify_pack_light_data();
  if (result != 0) {
    return result;
  }

  result = verify_cull_failure_zeroes_output();
  if (result != 0) {
    return result;
  }

  return verify_tile_texture_layout();
}

} // namespace

/// Runs this executable or test program.
int main() {
  // The tile-cap report is latched for the run, and the stress case below
  // overflows tiles too, so the sink listens from the start and the count
  // is taken at the end: however many cases and frames overflowed, the run
  // says so exactly once.
  TileCapLog tileCapLog{};
  const bool loggingReady = engine::core::initialize_logging();
  const bool sinkReady =
      loggingReady &&
      engine::core::log_register_sink(&count_tile_cap_reports, &tileCapLog);

  int result = run_cases();
  if (result == 0) {
    result = verify_tile_cap_keeps_the_first_lights();
  }

  if (sinkReady) {
    engine::core::log_unregister_sink(&count_tile_cap_reports, &tileCapLog);
  }
  if (loggingReady) {
    engine::core::shutdown_logging();
  }
  if (result != 0) {
    return result;
  }
  if (!sinkReady) {
    return 910;
  }
  // On base the drop was silent: zero reports.
  if (tileCapLog.reports != 1) {
    std::fprintf(stderr, "light_culling_test: the tile cap was reported %d "
                         "times in the run, expected once\n",
                 tileCapLog.reports);
    return 911;
  }
  return 0;
}
