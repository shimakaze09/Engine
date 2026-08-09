// Verifies manifold cache behavior for the Engine test suite: add/fill,
// overflow reduction, eviction, feature matching, reset, plus the issue
// #110 world-scoping and entity-generation safety contracts.

#include <cstdio>

#include "engine/math/vec3.h"
#include "engine/physics/constraint_solver.h"
#include "engine/physics/physics_context.h"

namespace {

namespace math = engine::math;
namespace physics = engine::physics;

physics::Entity make_entity(std::uint32_t index,
                            std::uint32_t generation) noexcept {
  physics::Entity entity{};
  entity.index = index;
  entity.generation = generation;
  return entity;
}

// Test: adding contacts up to kMaxContacts fills correctly.
int test_manifold_add_and_fill() noexcept {
  physics::PhysicsContext ctx{};
  physics::manifold_reset(ctx);

  const math::Vec3 normal(0.0F, 1.0F, 0.0F);
  const physics::Entity a = make_entity(1U, 1U);
  const physics::Entity b = make_entity(2U, 1U);

  for (std::uint32_t i = 0U; i < physics::ContactManifold::kMaxContacts; ++i) {
    const math::Vec3 pt(static_cast<float>(i) * 0.1F, 0.0F, 0.0F);
    physics::manifold_add_contact(ctx, a, b, pt, pt, normal,
                                  0.01F * static_cast<float>(i + 1U), i,
                                  /*frameNumber=*/1U);
  }

  if (physics::manifold_count(ctx) != 1U) {
    std::printf("FAIL add_and_fill: manifold_count=%zu (expected 1)\n",
                physics::manifold_count(ctx));
    return 1;
  }

  const physics::ContactManifold *m = physics::manifold_get(ctx, 0U);
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
  physics::PhysicsContext ctx{};
  physics::manifold_reset(ctx);

  const math::Vec3 normal(0.0F, 1.0F, 0.0F);
  const physics::Entity a = make_entity(1U, 1U);
  const physics::Entity b = make_entity(2U, 1U);

  // Add 5 contacts — should still end up with kMaxContacts.
  for (std::uint32_t i = 0U; i < physics::ContactManifold::kMaxContacts + 1U;
       ++i) {
    const math::Vec3 pt(static_cast<float>(i) * 0.2F, 0.0F,
                        static_cast<float>(i % 2U) * 0.1F);
    physics::manifold_add_contact(ctx, a, b, pt, pt, normal,
                                  0.01F * static_cast<float>(i + 1U), 100U + i,
                                  /*frameNumber=*/1U);
  }

  const physics::ContactManifold *m = physics::manifold_get(ctx, 0U);
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
  physics::PhysicsContext ctx{};
  physics::manifold_reset(ctx);

  const math::Vec3 pt(0.0F, 0.0F, 0.0F);
  const math::Vec3 normal(0.0F, 1.0F, 0.0F);

  // Add manifold in frame 5.
  physics::manifold_add_contact(ctx, make_entity(1U, 1U), make_entity(2U, 1U),
                                pt, pt, normal, 0.01F, 0U, 5U);
  // Add another manifold in frame 6.
  physics::manifold_add_contact(ctx, make_entity(3U, 1U), make_entity(4U, 1U),
                                pt, pt, normal, 0.01F, 0U, 6U);

  if (physics::manifold_count(ctx) != 2U) {
    return 1;
  }

  // Evict manifolds last used before frame 6.
  physics::manifold_evict_stale(ctx, 6U);

  if (physics::manifold_count(ctx) != 1U) {
    std::printf("FAIL evict_stale: count=%zu (expected 1)\n",
                physics::manifold_count(ctx));
    return 2;
  }
  return 0;
}

// Test: feature-ID matching updates existing contact in-place.
int test_manifold_feature_id_match() noexcept {
  physics::PhysicsContext ctx{};
  physics::manifold_reset(ctx);

  const math::Vec3 pt(1.0F, 0.0F, 0.0F);
  const math::Vec3 normal(0.0F, 1.0F, 0.0F);
  const physics::Entity a = make_entity(1U, 1U);
  const physics::Entity b = make_entity(2U, 1U);

  // Add initial contact with feature ID 42.
  physics::manifold_add_contact(ctx, a, b, pt, pt, normal, 0.05F, 42U, 1U);

  const physics::ContactManifold *m = physics::manifold_get(ctx, 0U);
  if (m == nullptr || m->contactCount != 1U) {
    return 1;
  }

  const math::Vec3 pt2(1.1F, 0.0F, 0.0F);
  physics::manifold_add_contact(ctx, a, b, pt2, pt2, normal, 0.08F, 42U, 2U);

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
  physics::PhysicsContext ctx{};
  physics::manifold_reset(ctx);

  const math::Vec3 pt(0.0F, 0.0F, 0.0F);
  const math::Vec3 normal(0.0F, 1.0F, 0.0F);
  physics::manifold_add_contact(ctx, make_entity(1U, 1U), make_entity(2U, 1U),
                                pt, pt, normal, 0.01F, 0U, 1U);
  physics::manifold_add_contact(ctx, make_entity(3U, 1U), make_entity(4U, 1U),
                                pt, pt, normal, 0.01F, 0U, 1U);

  if (physics::manifold_count(ctx) != 2U) {
    return 1;
  }

  physics::manifold_reset(ctx);
  if (physics::manifold_count(ctx) != 0U) {
    return 2;
  }
  return 0;
}

// Test: a reused entity index with a new generation must not inherit the
// old pair's manifold or its accumulated impulse.
int test_manifold_generation_reuse_isolated() noexcept {
  physics::PhysicsContext ctx{};
  physics::manifold_reset(ctx);

  const physics::Entity oldEntity = make_entity(7U, 1U);
  const physics::Entity partner = make_entity(2U, 1U);

  physics::ContactManifold *oldManifold =
      physics::manifold_acquire(ctx, oldEntity, partner, 1U);
  if (oldManifold == nullptr) {
    return 1;
  }
  oldManifold->contactCount = 1U;
  oldManifold->contacts[0U].accumulatedNormalImpulse = 5.0F;

  // Same index, new generation: must acquire a distinct fresh manifold.
  const physics::Entity reused = make_entity(7U, 2U);
  physics::ContactManifold *fresh =
      physics::manifold_acquire(ctx, reused, partner, 2U);
  if (fresh == nullptr) {
    return 2;
  }
  if (fresh == oldManifold) {
    std::printf("FAIL generation_reuse: stale manifold inherited\n");
    return 3;
  }
  if (fresh->contactCount != 0U) {
    return 4;
  }
  return 0;
}

// Test: two contexts never share manifolds (world-scoped storage).
int test_manifold_world_scoped() noexcept {
  physics::PhysicsContext ctxA{};
  physics::PhysicsContext ctxB{};
  physics::manifold_reset(ctxA);
  physics::manifold_reset(ctxB);

  const math::Vec3 pt(0.0F, 0.0F, 0.0F);
  const math::Vec3 normal(0.0F, 1.0F, 0.0F);
  physics::manifold_add_contact(ctxA, make_entity(1U, 1U), make_entity(2U, 1U),
                                pt, pt, normal, 0.01F, 0U, 1U);

  if (physics::manifold_count(ctxA) != 1U) {
    return 1;
  }
  if (physics::manifold_count(ctxB) != 0U) {
    std::printf("FAIL world_scoped: context B sees %zu manifolds\n",
                physics::manifold_count(ctxB));
    return 2;
  }
  return 0;
}

// Test: acquiring a stored pair in flipped order restarts the manifold
// (mirrored points/impulses must not replay from the wrong perspective).
int test_manifold_flipped_order_restarts() noexcept {
  physics::PhysicsContext ctx{};
  physics::manifold_reset(ctx);

  const physics::Entity a = make_entity(1U, 1U);
  const physics::Entity b = make_entity(2U, 1U);

  physics::ContactManifold *first = physics::manifold_acquire(ctx, a, b, 1U);
  if (first == nullptr) {
    return 1;
  }
  first->contactCount = 2U;

  physics::ContactManifold *flipped = physics::manifold_acquire(ctx, b, a, 2U);
  if (flipped != first) {
    return 2;
  }
  if (flipped->contactCount != 0U) {
    return 3;
  }
  if (!(flipped->entityA == b) || !(flipped->entityB == a)) {
    return 4;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
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
      {"manifold_generation_reuse_isolated",
       test_manifold_generation_reuse_isolated},
      {"manifold_world_scoped", test_manifold_world_scoped},
      {"manifold_flipped_order_restarts",
       test_manifold_flipped_order_restarts},
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
