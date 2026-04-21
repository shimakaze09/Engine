#include <cmath>
#include <cstdio>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/renderer/shadow_map.h"

namespace {

// ---------------------------------------------------------------------------
// Test 1: compute_point_shadow_matrices produces 6 distinct matrices.
// ---------------------------------------------------------------------------
int verify_point_matrices_distinct() {
  const engine::math::Vec3 pos(0.0F, 3.0F, 0.0F);
  engine::math::Mat4 vp[6]{};
  engine::renderer::compute_point_shadow_matrices(pos, 20.0F, vp);

  // Every pair of faces must produce a different matrix (at least one element
  // differs by more than epsilon).
  for (int a = 0; a < 6; ++a) {
    for (int b = a + 1; b < 6; ++b) {
      bool same = true;
      for (int c = 0; c < 4 && same; ++c) {
        for (int r = 0; r < 4 && same; ++r) {
          const float va = (&vp[a].columns[c].x)[r];
          const float vb = (&vp[b].columns[c].x)[r];
          if (std::abs(va - vb) > 1e-4F) {
            same = false;
          }
        }
      }
      if (same) {
        // Faces a and b produced identical matrices — wrong.
        return 100 + a * 10 + b;
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 2: None of the 6 face matrices is the identity.
// ---------------------------------------------------------------------------
int verify_point_matrices_not_identity() {
  const engine::math::Vec3 pos(1.0F, 1.0F, 1.0F);
  engine::math::Mat4 vp[6]{};
  engine::renderer::compute_point_shadow_matrices(pos, 10.0F, vp);

  for (int f = 0; f < 6; ++f) {
    bool isIdentity = true;
    for (int c = 0; c < 4 && isIdentity; ++c) {
      for (int r = 0; r < 4 && isIdentity; ++r) {
        const float expected = (c == r) ? 1.0F : 0.0F;
        const float val = (&vp[f].columns[c].x)[r];
        if (std::abs(val - expected) > 1e-6F) {
          isIdentity = false;
        }
      }
    }
    if (isIdentity) {
      return 200 + f; // Face f is identity — wrong
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 3: Different positions produce different matrices (translation encoded).
// ---------------------------------------------------------------------------
int verify_point_matrices_position_sensitivity() {
  engine::math::Mat4 vpA[6]{};
  engine::math::Mat4 vpB[6]{};
  engine::renderer::compute_point_shadow_matrices(engine::math::Vec3(0, 0, 0),
                                                  10.0F, vpA);
  engine::renderer::compute_point_shadow_matrices(engine::math::Vec3(5, 0, 0),
                                                  10.0F, vpB);

  // At least one face must differ.
  bool anyDiff = false;
  for (int f = 0; f < 6 && !anyDiff; ++f) {
    for (int c = 0; c < 4 && !anyDiff; ++c) {
      for (int r = 0; r < 4 && !anyDiff; ++r) {
        const float va = (&vpA[f].columns[c].x)[r];
        const float vb = (&vpB[f].columns[c].x)[r];
        if (std::abs(va - vb) > 1e-4F) {
          anyDiff = true;
        }
      }
    }
  }
  if (!anyDiff) {
    return 300;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 4: PointShadowState defaults — not initialized, all indices -1.
// ---------------------------------------------------------------------------
int verify_point_state_defaults() {
  engine::renderer::PointShadowState state{};

  if (state.initialized) {
    return 400;
  }
  for (std::size_t i = 0U; i < engine::renderer::kMaxPointShadowLights; ++i) {
    if (state.slots[i].lightIndex != -1) {
      return 401;
    }
    if (state.slots[i].depthCubemap != 0U) {
      return 402;
    }
    if (state.slots[i].depthFbo != 0U) {
      return 403;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 5: kMaxPointShadowLights is 4 and kPointShadowMapResolution is 512.
// ---------------------------------------------------------------------------
int verify_point_shadow_constants() {
  if (engine::renderer::kMaxPointShadowLights != 4U) {
    return 500;
  }
  if (engine::renderer::kPointShadowMapResolution != 512) {
    return 501;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Test 6: PointShadowData — 6 face slots, far plane and index defaults.
// ---------------------------------------------------------------------------
int verify_point_data_defaults() {
  engine::renderer::PointShadowData data{};

  if (data.lightIndex != -1) {
    return 600;
  }
  if (data.farPlane != 0.0F) {
    return 601;
  }
  if (data.depthCubemap != 0U) {
    return 602;
  }
  if (data.depthFbo != 0U) {
    return 603;
  }
  return 0;
}

} // namespace

int main() {
  int result = verify_point_matrices_distinct();
  if (result != 0)
    return result;

  result = verify_point_matrices_not_identity();
  if (result != 0)
    return result;

  result = verify_point_matrices_position_sensitivity();
  if (result != 0)
    return result;

  result = verify_point_state_defaults();
  if (result != 0)
    return result;

  result = verify_point_shadow_constants();
  if (result != 0)
    return result;

  result = verify_point_data_defaults();
  if (result != 0)
    return result;

  return 0;
}
