// Verifies light culling test behavior for the Engine test suite.

#include <cmath>
#include <cstddef>
#include <vector>

#include "engine/renderer/command_buffer.h"
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
  const float expectedSpot0[13] = {10.0F, 20.0F, 30.0F, 0.0F,  -1.0F,
                                   0.0F,  0.75F, 0.5F,  0.25F, 6.0F,
                                   15.0F, 0.25F, 0.5F};
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

} // namespace

/// Runs this executable or test program.
int main() {
  int result = verify_empty_scene_culling();
  if (result != 0) {
    return result;
  }

  result = verify_single_point_light_center();
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

  return verify_pack_light_data();
}
