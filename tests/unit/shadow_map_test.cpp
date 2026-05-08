#include <cmath>
#include <cstdio>

#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/renderer/shadow_map.h"

namespace {

// ---------------------------------------------------------------------------
// Test 1: Cascade splits — distances monotonically increase.
// ---------------------------------------------------------------------------
int verify_cascade_splits_monotonic() {
  const auto splits =
      engine::renderer::compute_cascade_splits(0.1F, 100.0F, 0.75F);

  // First distance must equal near clip.
  if (std::abs(splits.distances[0] - 0.1F) > 1e-5F) {
    return 100;
  }
  // Last distance must equal far clip.
  if (std::abs(splits.distances[engine::renderer::kShadowCascadeCount] -
               100.0F) > 1e-5F) {
    return 101;
  }

  // All distances must be strictly increasing.
  for (std::size_t i = 1U; i <= engine::renderer::kShadowCascadeCount; ++i) {
    if (splits.distances[i] <= splits.distances[i - 1]) {
      return 102;
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 2: Cascade splits with lambda=0 (uniform) are evenly spaced.
// ---------------------------------------------------------------------------
int verify_cascade_splits_uniform() {
  const auto splits =
      engine::renderer::compute_cascade_splits(0.1F, 100.0F, 0.0F);

  const float range = 100.0F - 0.1F;
  const float expectedStep = range / 4.0F; // 4 cascades

  for (std::size_t i = 1U; i < engine::renderer::kShadowCascadeCount; ++i) {
    const float expected = 0.1F + expectedStep * static_cast<float>(i);
    if (std::abs(splits.distances[i] - expected) > 1e-3F) {
      return 200;
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 3: Cascade splits with lambda=1 (logarithmic).
// ---------------------------------------------------------------------------
int verify_cascade_splits_logarithmic() {
  const auto splits =
      engine::renderer::compute_cascade_splits(0.1F, 100.0F, 1.0F);

  // Logarithmic splits should place more splits near the camera.
  // First cascade should be smaller than uniform spacing.
  const float uniformFirst = 0.1F + (100.0F - 0.1F) / 4.0F;
  if (splits.distances[1] >= uniformFirst) {
    return 300; // Should be closer to near than uniform
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 4: Cascade matrix produces a valid orthographic projection.
// ---------------------------------------------------------------------------
int verify_cascade_matrix_valid() {
  const engine::math::Mat4 viewMat = engine::math::look_at(
      engine::math::Vec3(0, 5, 10), engine::math::Vec3(0, 0, 0),
      engine::math::Vec3(0, 1, 0));
  const engine::math::Mat4 projMat =
      engine::math::perspective(1.047F, 16.0F / 9.0F, 0.1F, 100.0F);

  const engine::math::Vec3 lightDir(0.0F, -1.0F, -0.5F);

  const auto matrix = engine::renderer::compute_cascade_matrix(
      viewMat, projMat, lightDir, 0.1F, 25.0F,
      engine::renderer::kShadowMapResolution);

  // The matrix should not be identity (that would mean computation failed).
  bool isIdentity = true;
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      float expected = (c == r) ? 1.0F : 0.0F;
      float val = (&matrix.columns[c].x)[r];
      if (std::abs(val - expected) > 1e-6F) {
        isIdentity = false;
        break;
      }
    }
    if (!isIdentity)
      break;
  }

  if (isIdentity) {
    return 400;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 5: Snap-to-texel produces stable results.
// ---------------------------------------------------------------------------
int verify_snap_to_texel_stable() {
  const engine::math::Mat4 viewMat = engine::math::look_at(
      engine::math::Vec3(0, 5, 10), engine::math::Vec3(0, 0, 0),
      engine::math::Vec3(0, 1, 0));
  const engine::math::Mat4 projMat =
      engine::math::perspective(1.047F, 16.0F / 9.0F, 0.1F, 100.0F);

  const engine::math::Vec3 lightDir(0.0F, -1.0F, -0.5F);

  const auto matrix = engine::renderer::compute_cascade_matrix(
      viewMat, projMat, lightDir, 0.1F, 25.0F,
      engine::renderer::kShadowMapResolution);

  const auto snapped = engine::renderer::snap_to_texel(matrix, 1024);

  // Snapped x/y translation should be aligned to texel grid.
  const float texelWorld = 2.0F / 1024.0F;
  const float tx = snapped.columns[3].x;
  const float ty = snapped.columns[3].y;
  const float txRemainder = std::fmod(std::abs(tx), texelWorld);
  const float tyRemainder = std::fmod(std::abs(ty), texelWorld);

  if (txRemainder > 1e-5F && std::abs(txRemainder - texelWorld) > 1e-5F) {
    return 500;
  }
  if (tyRemainder > 1e-5F && std::abs(tyRemainder - texelWorld) > 1e-5F) {
    return 501;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 6: Cascade matrix remains stable for sub-texel camera movement.
// ---------------------------------------------------------------------------
int verify_cascade_matrix_stable_for_sub_texel_motion() {
  const engine::math::Vec3 lightDir(0.0F, -1.0F, -0.5F);
  const engine::math::Mat4 projMat =
      engine::math::perspective(1.047F, 16.0F / 9.0F, 0.1F, 100.0F);

  const engine::math::Mat4 viewA = engine::math::look_at(
      engine::math::Vec3(0.0F, 5.0F, 10.0F),
      engine::math::Vec3(0.0F, 0.0F, 0.0F),
      engine::math::Vec3(0.0F, 1.0F, 0.0F));
  const engine::math::Mat4 viewB = engine::math::look_at(
      engine::math::Vec3(0.0001F, 5.0F, 10.0F),
      engine::math::Vec3(0.0001F, 0.0F, 0.0F),
      engine::math::Vec3(0.0F, 1.0F, 0.0F));

  const auto matrixA = engine::renderer::compute_cascade_matrix(
      viewA, projMat, lightDir, 0.1F, 25.0F,
      engine::renderer::kShadowMapResolution);
  const auto matrixB = engine::renderer::compute_cascade_matrix(
      viewB, projMat, lightDir, 0.1F, 25.0F,
      engine::renderer::kShadowMapResolution);

  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      const float a = (&matrixA.columns[c].x)[r];
      const float b = (&matrixB.columns[c].x)[r];
      if (std::abs(a - b) > 1e-5F) {
        return 700;
      }
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 7: Distant cascades use lower shadow resolutions.
// ---------------------------------------------------------------------------
int verify_shadow_cascade_lod_resolutions() {
  const int first = engine::renderer::shadow_cascade_resolution(0U);
  const int last = engine::renderer::shadow_cascade_resolution(
      engine::renderer::kShadowCascadeCount - 1U);

  if (first != engine::renderer::kShadowMapResolution) {
    return 800;
  }
  if (last >= first) {
    return 801;
  }

  int previous = first;
  for (std::size_t i = 0U; i < engine::renderer::kShadowCascadeCount; ++i) {
    const int resolution = engine::renderer::shadow_cascade_resolution(i);
    if (resolution <= 0) {
      return 802;
    }
    if ((resolution & (resolution - 1)) != 0) {
      return 803;
    }
    if (resolution > previous) {
      return 804;
    }
    previous = resolution;
  }

  if (engine::renderer::shadow_cascade_resolution(
          engine::renderer::kShadowCascadeCount + 10U) != last) {
    return 805;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 8: ShadowMapState initialization without GPU (no crash).
// ---------------------------------------------------------------------------
int verify_shadow_state_defaults() {
  engine::renderer::ShadowMapState state{};

  if (state.initialized) {
    return 600;
  }
  for (std::size_t i = 0U; i < engine::renderer::kShadowCascadeCount; ++i) {
    if (state.depthTextures[i] != 0U) {
      return 601;
    }
    if (state.depthFbos[i] != 0U) {
      return 602;
    }
    if (state.resolutions[i] != 0) {
      return 603;
    }
  }

  return 0;
}

} // namespace

int main() {
  int result = verify_cascade_splits_monotonic();
  if (result != 0)
    return result;

  result = verify_cascade_splits_uniform();
  if (result != 0)
    return result;

  result = verify_cascade_splits_logarithmic();
  if (result != 0)
    return result;

  result = verify_cascade_matrix_valid();
  if (result != 0)
    return result;

  result = verify_snap_to_texel_stable();
  if (result != 0)
    return result;

  result = verify_cascade_matrix_stable_for_sub_texel_motion();
  if (result != 0)
    return result;

  result = verify_shadow_cascade_lod_resolutions();
  if (result != 0)
    return result;

  result = verify_shadow_state_defaults();
  if (result != 0)
    return result;

  return 0;
}
