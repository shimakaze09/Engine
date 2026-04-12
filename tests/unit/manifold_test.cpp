#include <cstdio>

#include "engine/math/vec3.h"
#include "engine/physics/constraint_solver.h"

namespace {

namespace math = engine::math;
namespace physics = engine::physics;

// Test: adding contacts up to kMaxContacts fills correctly.
int test_manifold_add_and_fill() noexcept {
  physics::manifold_reset();

  const math::Vec3 normal(0.0F, 1.0F, 0.0F);

  for (std::uint32_t i = 0U; i < physics::ContactManifold::kMaxContacts;
       ++i) {
    const math::Vec3 pt(static_cast<float>(i) * 0.1F, 0.0F, 0.0F);
    physics::manifold_add_contact(0U, 1U, pt, pt, normal,
                                  0.01F * static_cast<float>(i + 1U), i,
                                  /*frameNumber=*/1U);
  }

  if (physics::manifold_count() != 1U) {
    std::printf("FAIL add_and_fill: manifold_count=%zu (expected 1)\n",
                physics::manifold_count());
    return 1;
  }

  const physics::ContactManifold *m = physics::manifold_get(0U);
  if (m == nullptr) {
    return 2;
  }
  if (m->contactCount != physics::ContactManifold::kMaxContacts) {
    std::printf("FAIL add_and_fill: contactCount=%zu (expected %zu)\n",
                m->contactCount, physics::ContactManifold::kMaxContacts);
    return 3;
  }
  return 0;
}

// Test: adding a 5th contact triggers reduction to kMaxContacts.
int test_manifold_overflow_reduces() noexcept {
  physics::manifold_reset();

  const math::Vec3 normal(0.0F, 1.0F, 0.0F);

  // Add 5 contacts — should still end up with kMaxContacts.
  for (std::uint32_t i = 0U;
       i < physics::ContactManifold::kMaxContacts + 1U; ++i) {
    const math::Vec3 pt(static_cast<float>(i) * 0.2F, 0.0F,
                        static_cast<float>(i % 2U) * 0.1F);
    physics::manifold_add_contact(0U, 1U, pt, pt, normal,
                                  0.01F * static_cast<float>(i + 1U),
                                  100U + i, /*frameNumber=*/1U);
  }

  const physics::ContactManifold *m = physics::manifold_get(0U);
  if (m == nullptr) {
    return 1;
  }
  if (m->contactCount != physics::ContactManifold::kMaxContacts) {
    std::printf("FAIL overflow_reduces: contactCount=%zu (expected %zu)\n",
                m->contactCount, physics::ContactManifold::kMaxContacts);
    return 2;
  }
  return 0;
}

// Test: evict_stale removes manifolds not used in current frame.
int test_manifold_evict_stale() noexcept {
  physics::manifold_reset();

  const math::Vec3 pt(0.0F, 0.0F, 0.0F);
  const math::Vec3 normal(0.0F, 1.0F, 0.0F);

  // Add manifold in frame 5.
  physics::manifold_add_contact(0U, 1U, pt, pt, normal, 0.01F, 0U, 5U);
  // Add another manifold in frame 6.
  physics::manifold_add_contact(2U, 3U, pt, pt, normal, 0.01F, 0U, 6U);

  if (physics::manifold_count() != 2U) {
    return 1;
  }

  // Evict manifolds last used before frame 6.
  physics::manifold_evict_stale(6U);

  if (physics::manifold_count() != 1U) {
    std::printf("FAIL evict_stale: count=%zu (expected 1)\n",
                physics::manifold_count());
    return 2;
  }
  return 0;
}

// Test: feature-ID matching updates existing contact in-place.
int test_manifold_feature_id_match() noexcept {
  physics::manifold_reset();

  const math::Vec3 pt(1.0F, 0.0F, 0.0F);
  const math::Vec3 normal(0.0F, 1.0F, 0.0F);

  // Add initial contact with feature ID 42.
  physics::manifold_add_contact(0U, 1U, pt, pt, normal, 0.05F, 42U, 1U);

  const physics::ContactManifold *m = physics::manifold_get(0U);
  if (m == nullptr || m->contactCount != 1U) {
    return 1;
  }

  // Update same contact (same entity pair + same feature ID).
  const math::Vec3 pt2(1.1F, 0.0F, 0.0F);
  physics::manifold_add_contact(0U, 1U, pt2, pt2, normal, 0.08F, 42U, 2U);

  // Should still have 1 contact, not 2.
  if (m->contactCount != 1U) {
    std::printf("FAIL feature_id_match: contactCount=%zu (expected 1)\n",
                m->contactCount);
    return 2;
  }

  // The contact should have the updated penetration.
  if (m->contacts[0U].penetration < 0.07F) {
    std::printf("FAIL feature_id_match: penetration=%.3f (expected ~0.08)\n",
                m->contacts[0U].penetration);
    return 3;
  }
  return 0;
}

// Test: reset clears all manifolds.
int test_manifold_reset() noexcept {
  physics::manifold_reset();

  const math::Vec3 pt(0.0F, 0.0F, 0.0F);
  const math::Vec3 normal(0.0F, 1.0F, 0.0F);
  physics::manifold_add_contact(0U, 1U, pt, pt, normal, 0.01F, 0U, 1U);
  physics::manifold_add_contact(2U, 3U, pt, pt, normal, 0.01F, 0U, 1U);

  if (physics::manifold_count() != 2U) {
    return 1;
  }

  physics::manifold_reset();
  if (physics::manifold_count() != 0U) {
    return 2;
  }
  return 0;
}

} // namespace

int main() {
  struct TestCase {
    const char *name;
    int (*func)();
  };

  const TestCase tests[] = {
      {"manifold_add_and_fill", test_manifold_add_and_fill},
      {"manifold_overflow_reduces", test_manifold_overflow_reduces},
      {"manifold_evict_stale", test_manifold_evict_stale},
      {"manifold_feature_id_match", test_manifold_feature_id_match},
      {"manifold_reset", test_manifold_reset},
  };

  int failures = 0;
  for (const auto &tc : tests) {
    const int result = tc.func();
    if (result != 0) {
      std::printf("FAIL %s (code %d)\n", tc.name, result);
      ++failures;
    } else {
      std::printf("PASS %s\n", tc.name);
    }
  }

  if (failures > 0) {
    std::printf("%d test(s) failed\n", failures);
    return 1;
  }
  std::printf("All manifold tests passed\n");
  return 0;
}
