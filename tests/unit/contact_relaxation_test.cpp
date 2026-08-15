// Regression tests for issue #123: single-point contact paths (sphere/
// capsule fast paths, degenerate-clip fallbacks) must enter the persistent
// manifold cache like clipped multi-point manifolds, and the
// physics.contact_relaxation_iterations cvar must gate the outer relaxation
// pass introduced to converge resting-stack residual velocity.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/core/cvar.h"
#include "engine/math/vec3.h"
#include "engine/physics/constraint_solver.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

static int g_passed = 0;
static int g_failed = 0;

static void check(bool condition, const char *name) noexcept {
  if (condition) {
    ++g_passed;
    std::printf("  PASS: %s\n", name);
  } else {
    ++g_failed;
    std::printf("  FAIL: %s\n", name);
  }
}

static void run_step(engine::runtime::World &world) noexcept {
  world.begin_update_phase();
  engine::runtime::step_physics(world, 1.0F / 60.0F);
  engine::runtime::resolve_collisions(world);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
}

static void add_floor(engine::runtime::World &world) noexcept {
  const auto floor = world.create_entity();
  engine::runtime::Transform floorT{};
  world.add_transform(floor, floorT);
  engine::runtime::Collider floorCol{};
  floorCol.halfExtents = engine::math::Vec3(10.0F, 0.5F, 10.0F);
  world.add_collider(floor, floorCol);
  engine::runtime::RigidBody floorRB{};
  floorRB.inverseMass = 0.0F;
  world.add_rigid_body(floor, floorRB);
}

static engine::core::Entity add_box(engine::runtime::World &world,
                                    float y) noexcept {
  const auto box = world.create_entity();
  engine::runtime::Transform boxT{};
  boxT.position = engine::math::Vec3(0.0F, y, 0.0F);
  world.add_transform(box, boxT);
  engine::runtime::Collider boxCol{};
  boxCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world.add_collider(box, boxCol);
  engine::runtime::RigidBody boxRB{};
  boxRB.inverseMass = 1.0F;
  world.add_rigid_body(box, boxRB);
  return box;
}

/// A sphere resting on a static AABB floor (narrow_phase_aabb_sphere, a
/// single-point path with no clipped manifold) must still populate the
/// persistent manifold cache after a production resolve: before this fix,
/// only clipped multi-point manifolds (resolve_manifold_contact) touched
/// the cache, so a resting sphere never warm-started and never joined the
/// outer relaxation pass.
static void test_sphere_on_floor_enters_manifold_cache() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();

  add_floor(*world);
  const auto sphere = world->create_entity();
  engine::runtime::Transform sphereT{};
  sphereT.position = engine::math::Vec3(0.0F, 0.99F, 0.0F);
  world->add_transform(sphere, sphereT);
  engine::runtime::Collider sphereCol{};
  sphereCol.shape = engine::runtime::ColliderShape::Sphere;
  sphereCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(sphere, sphereCol);
  engine::runtime::RigidBody sphereRB{};
  sphereRB.inverseMass = 1.0F;
  world->add_rigid_body(sphere, sphereRB);

  for (int i = 0; i < 10; ++i) {
    run_step(*world);
  }

  const engine::physics::PhysicsContext &ctx = world->physics_context();
  check(engine::physics::manifold_count(ctx) > 0U,
        "Production resolve populates the manifold cache for a "
        "single-point sphere-vs-floor contact");

  bool sphereManifold = false;
  bool singlePoint = false;
  bool warmImpulse = false;
  for (std::size_t m = 0U; m < engine::physics::manifold_count(ctx); ++m) {
    const engine::physics::ContactManifold *manifold =
        engine::physics::manifold_get(ctx, m);
    if (manifold == nullptr) {
      continue;
    }
    if ((manifold->entityA == sphere) || (manifold->entityB == sphere)) {
      sphereManifold = true;
      singlePoint = manifold->contactCount == 1U;
      for (std::size_t c = 0U; c < manifold->contactCount; ++c) {
        if (manifold->contacts[c].accumulatedNormalImpulse > 0.0F) {
          warmImpulse = true;
        }
      }
    }
  }
  check(sphereManifold, "Cache holds the sphere-vs-floor pair keyed by entity");
  check(singlePoint, "Sphere-vs-floor cache entry is a single point");
  check(warmImpulse, "Cached single-point contact carries a solved impulse");
}

/// physics.contact_relaxation_iterations registers with the documented
/// default of 4.
static void test_relaxation_cvar_default() noexcept {
  engine::physics::register_physics_cvars();
  const int value =
      engine::core::cvar_get_int("physics.contact_relaxation_iterations", -1);
  check(value == 4, "contact_relaxation_iterations registers with default 4");
}

/// Setting physics.contact_relaxation_iterations to 0 reproduces the
/// pre-#123 single-pass behavior: a resting stack's residual velocity stays
/// bounded but never converges to sleep within the same step budget that
/// converges it at the default (proving the cvar genuinely gates the extra
/// passes rather than always running them).
static void test_zero_iterations_disables_relaxation() noexcept {
  engine::physics::register_physics_cvars();
  const int savedValue =
      engine::core::cvar_get_int("physics.contact_relaxation_iterations", 4);

  if (!engine::core::cvar_set_int("physics.contact_relaxation_iterations",
                                  0)) {
    check(false, "Setting contact_relaxation_iterations to 0 failed");
    return;
  }

  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    engine::core::cvar_set_int("physics.contact_relaxation_iterations",
                               savedValue);
    return;
  }
  world->end_frame_phase();

  add_floor(*world);
  const engine::core::Entity boxes[3] = {
      add_box(*world, 1.0F), add_box(*world, 2.001F), add_box(*world, 3.002F)};

  // Same total step budget (150 + 30 + 60) the enabled-by-default stack test
  // uses to reach sleep; with relaxation off this must NOT be enough.
  for (int i = 0; i < 240; ++i) {
    run_step(*world);
  }

  engine::core::cvar_set_int("physics.contact_relaxation_iterations",
                             savedValue);

  bool anyAwake = false;
  for (const auto &box : boxes) {
    const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(box);
    if ((body == nullptr) || !body->sleeping) {
      anyAwake = true;
    }
  }
  check(anyAwake,
        "physics.contact_relaxation_iterations=0 reproduces the pre-#123 "
        "single-pass behavior (stack never fully sleeps)");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== Contact Relaxation Tests (issue #123) ===\n");

  test_sphere_on_floor_enters_manifold_cache();
  test_relaxation_cvar_default();
  test_zero_iterations_disables_relaxation();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
