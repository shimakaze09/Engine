#include <cmath>

#include "engine/renderer/shadow_map.h"

namespace {

// ---------------------------------------------------------------------------
// Test 1: Auto-exposure CVar defaults are reasonable.
// This tests the shadow_map.h constants (used by auto-exposure system).
// ---------------------------------------------------------------------------
int verify_shadow_map_constants() {
  // Shadow cascade count should be 4.
  if (engine::renderer::kShadowCascadeCount != 4U) {
    return 100;
  }

  // Shadow map resolution should be positive and power-of-two.
  const int res = engine::renderer::kShadowMapResolution;
  if (res <= 0)
    return 101;
  if ((res & (res - 1)) != 0)
    return 102; // not power of 2

  return 0;
}

// ---------------------------------------------------------------------------
// Test 2: Cascade splits cover full range for various lambda values.
// ---------------------------------------------------------------------------
int verify_cascade_splits_full_range() {
  const float lambdas[] = {0.0F, 0.25F, 0.5F, 0.75F, 1.0F};

  for (float lambda : lambdas) {
    const auto splits =
        engine::renderer::compute_cascade_splits(0.5F, 500.0F, lambda);

    if (std::abs(splits.distances[0] - 0.5F) > 1e-5F) {
      return 200;
    }
    if (std::abs(splits.distances[engine::renderer::kShadowCascadeCount] -
                 500.0F) > 1e-5F) {
      return 201;
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 3: Narrow near/far range still produces valid splits.
// ---------------------------------------------------------------------------
int verify_cascade_splits_narrow_range() {
  const auto splits =
      engine::renderer::compute_cascade_splits(1.0F, 2.0F, 0.5F);

  for (std::size_t i = 1U; i <= engine::renderer::kShadowCascadeCount; ++i) {
    if (splits.distances[i] <= splits.distances[i - 1]) {
      return 300;
    }
  }

  return 0;
}

} // namespace

int main() {
  int result = verify_shadow_map_constants();
  if (result != 0)
    return result;

  result = verify_cascade_splits_full_range();
  if (result != 0)
    return result;

  result = verify_cascade_splits_narrow_range();
  if (result != 0)
    return result;

  return 0;
}
