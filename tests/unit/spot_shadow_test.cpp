#include <cmath>
#include <cstdio>

#ifdef _MSC_VER
#pragma warning(disable : 4127) // constant conditional (constexpr checks in tests)
#endif

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/renderer/shadow_map.h"

namespace {

// ---------------------------------------------------------------------------
// Test 1: Spot shadow matrix is not identity.
// ---------------------------------------------------------------------------
int verify_spot_matrix_not_identity() {
  const engine::math::Vec3 pos(0.0F, 5.0F, 0.0F);
  const engine::math::Vec3 dir(0.0F, -1.0F, 0.0F);
  const float outerCone = 0.4F; // ~23 degrees
  const float radius = 20.0F;

  const engine::math::Mat4 m =
      engine::renderer::compute_spot_shadow_matrix(pos, dir, outerCone, radius);

  bool isIdentity = true;
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      const float expected = (c == r) ? 1.0F : 0.0F;
      const float val = (&m.columns[c].x)[r];
      if (std::abs(val - expected) > 1e-6F) {
        isIdentity = false;
        break;
      }
    }
    if (!isIdentity)
      break;
  }

  if (isIdentity) {
    return 100;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 2: Different positions produce different matrices.
// ---------------------------------------------------------------------------
int verify_spot_matrix_position_sensitivity() {
  const engine::math::Vec3 dir(0.0F, -1.0F, 0.0F);
  const engine::math::Mat4 m1 = engine::renderer::compute_spot_shadow_matrix(
      engine::math::Vec3(0, 5, 0), dir, 0.4F, 20.0F);
  const engine::math::Mat4 m2 = engine::renderer::compute_spot_shadow_matrix(
      engine::math::Vec3(10, 5, 0), dir, 0.4F, 20.0F);

  // Translation columns must differ.
  if (std::abs(m1.columns[3].x - m2.columns[3].x) < 1e-4F &&
      std::abs(m1.columns[3].y - m2.columns[3].y) < 1e-4F &&
      std::abs(m1.columns[3].z - m2.columns[3].z) < 1e-4F) {
    return 200;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 3: Wider cone angle produces different (wider) projection.
// ---------------------------------------------------------------------------
int verify_spot_matrix_cone_sensitivity() {
  const engine::math::Vec3 pos(0.0F, 5.0F, 0.0F);
  const engine::math::Vec3 dir(0.0F, -1.0F, 0.0F);

  const engine::math::Mat4 narrow =
      engine::renderer::compute_spot_shadow_matrix(pos, dir, 0.2F, 20.0F);
  const engine::math::Mat4 wide =
      engine::renderer::compute_spot_shadow_matrix(pos, dir, 0.6F, 20.0F);

  // Projection scale differs — any element must differ between narrow and wide.
  bool anyDiff = false;
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      if (std::abs((&narrow.columns[c].x)[r] - (&wide.columns[c].x)[r]) >
          1e-4F) {
        anyDiff = true;
        break;
      }
    }
    if (anyDiff) {
      break;
    }
  }
  if (!anyDiff) {
    return 300;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 4: SpotShadowState defaults — not initialized, all indices -1.
// ---------------------------------------------------------------------------
int verify_spot_state_defaults() {
  engine::renderer::SpotShadowState state{};

  if (state.initialized) {
    return 400;
  }
  for (std::size_t i = 0U; i < engine::renderer::kMaxSpotShadowLights; ++i) {
    if (state.slots[i].lightIndex != -1) {
      return 401;
    }
    if (state.slots[i].depthTexture != 0U) {
      return 402;
    }
    if (state.slots[i].depthFbo != 0U) {
      return 403;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 5: kMaxSpotShadowLights is 4 and kSpotShadowMapResolution is 512.
// ---------------------------------------------------------------------------
int verify_spot_shadow_constants() {
  if (engine::renderer::kMaxSpotShadowLights != 4U) {
    return 500;
  }
  if (engine::renderer::kSpotShadowMapResolution != 512) {
    return 501;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 6: SpotShadowData far plane defaults to zero.
// ---------------------------------------------------------------------------
int verify_spot_data_far_plane_default() {
  engine::renderer::SpotShadowData data{};
  if (data.farPlane != 0.0F) {
    return 600;
  }
  return 0;
}

} // namespace

int main() {
  int result = verify_spot_matrix_not_identity();
  if (result != 0)
    return result;

  result = verify_spot_matrix_position_sensitivity();
  if (result != 0)
    return result;

  result = verify_spot_matrix_cone_sensitivity();
  if (result != 0)
    return result;

  result = verify_spot_state_defaults();
  if (result != 0)
    return result;

  result = verify_spot_shadow_constants();
  if (result != 0)
    return result;

  result = verify_spot_data_far_plane_default();
  if (result != 0)
    return result;

  return 0;
}
