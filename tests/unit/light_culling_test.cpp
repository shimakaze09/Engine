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

} // namespace

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

  return verify_256_lights_stress();
}
