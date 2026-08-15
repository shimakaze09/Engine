// Regression tests for issue #110: the production resolve_collisions path
// must feed the world-scoped persistent contact-manifold cache — seeding
// solver impulses from it, writing solved impulses back, evicting stale
// pairs, and never letting a reused entity index inherit old contacts.
// test_stack_settles_without_jitter also covers issue #123's approved
// contract migration: resting-stack residual velocity must converge to
// (near) zero and actually sleep, not merely stay bounded.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/physics/constraint_solver.h"
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

// Runs one production fixed step: integrate, resolve, commit.
static void run_step(engine::runtime::World &world) noexcept {
  world.begin_update_phase();
  engine::runtime::step_physics(world, 1.0F / 60.0F);
  engine::runtime::resolve_collisions(world);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
}

// Adds a static floor box at the origin.
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

// Adds a dynamic unit box at the given height.
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

/// The production resolve must populate the persistent manifold cache and
/// carry solved impulses across steps (a resting box needs a gravity-
/// countering normal impulse every step, so its cached value is positive).
static void test_production_populates_manifold_cache() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();

  add_floor(*world);
  const engine::core::Entity box = add_box(*world, 0.99F);

  for (int i = 0; i < 10; ++i) {
    run_step(*world);
  }

  const engine::physics::PhysicsContext &ctx = world->physics_context();
  check(engine::physics::manifold_count(ctx) > 0U,
        "Production resolve populates the manifold cache");

  bool boxManifold = false;
  bool warmImpulse = false;
  for (std::size_t m = 0U; m < engine::physics::manifold_count(ctx); ++m) {
    const engine::physics::ContactManifold *manifold =
        engine::physics::manifold_get(ctx, m);
    if (manifold == nullptr) {
      continue;
    }
    if ((manifold->entityA == box) || (manifold->entityB == box)) {
      boxManifold = true;
      for (std::size_t c = 0U; c < manifold->contactCount; ++c) {
        if (manifold->contacts[c].accumulatedNormalImpulse > 0.0F) {
          warmImpulse = true;
        }
      }
    }
  }
  check(boxManifold, "Cache holds the resting pair keyed by entity");
  check(warmImpulse, "Cached contacts carry solved impulses across steps");

  // The cached impulse must survive into the following step, not reset.
  run_step(*world);
  bool survivingImpulse = false;
  for (std::size_t m = 0U; m < engine::physics::manifold_count(ctx); ++m) {
    const engine::physics::ContactManifold *manifold =
        engine::physics::manifold_get(ctx, m);
    if ((manifold != nullptr) &&
        ((manifold->entityA == box) || (manifold->entityB == box))) {
      for (std::size_t c = 0U; c < manifold->contactCount; ++c) {
        if (manifold->contacts[c].accumulatedNormalImpulse > 0.0F) {
          survivingImpulse = true;
        }
      }
    }
  }
  check(survivingImpulse, "Cached impulse survives into the following step");
}

/// A three-box stack under gravity must settle without positional jitter,
/// and its residual velocity must actually CONVERGE instead of merely
/// staying bounded.
///
/// OLD contract (pre-#123, defective, pinned by this test until now): the
/// solver made a single pass over the pair set per step, so each solved
/// pair could re-inject up to one step's worth of gravity into its lower
/// body every step; residual velocity stayed bounded (|v| <=
/// pairCount * g * dt ~= 0.49 m/s, asserted as energy <= 0.25) but never
/// reached zero, so a stacked box could never cross the sleep threshold.
///
/// NEW contract (issue #123's approved defective-contract migration):
/// physics.contact_relaxation_iterations' extra outer passes over the
/// manifold cache (default 4) propagate corrections through the stack
/// within one step instead of leaving convergence to accumulate one frame
/// at a time, so residual energy converges to (near) zero and every box in
/// the stack actually sleeps.
static void test_stack_settles_without_jitter() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();

  add_floor(*world);
  const engine::core::Entity boxes[3] = {
      add_box(*world, 1.0F), add_box(*world, 2.001F), add_box(*world, 3.002F)};

  for (int i = 0; i < 150; ++i) {
    run_step(*world);
  }
  float settledY[3] = {};
  for (int b = 0; b < 3; ++b) {
    engine::runtime::Transform t{};
    if (!world->get_transform(boxes[b], &t)) {
      check(false, "Stack transform lookup failed");
      return;
    }
    settledY[b] = t.position.y;
  }

  bool positionsSteady = true;
  for (int i = 0; i < 30; ++i) {
    run_step(*world);
    for (int b = 0; b < 3; ++b) {
      engine::runtime::Transform t{};
      if (!world->get_transform(boxes[b], &t) ||
          (std::fabs(t.position.y - settledY[b]) > 1.0e-3F)) {
        positionsSteady = false;
      }
    }
  }

  // kSleepFramesRequired (physics.cpp) is 60 consecutive low-energy steps;
  // run enough further steps that every box's sleep counter can fully
  // elapse even if it converged only near the end of the steady window
  // above.
  for (int i = 0; i < 60; ++i) {
    run_step(*world);
  }

  bool heightsHeld = true;
  bool residualConverged = true;
  bool stackSleeps = true;
  for (int b = 0; b < 3; ++b) {
    engine::runtime::Transform t{};
    const engine::runtime::RigidBody *body =
        world->get_rigid_body_ptr(boxes[b]);
    if (!world->get_transform(boxes[b], &t) || (body == nullptr)) {
      heightsHeld = false;
      residualConverged = false;
      stackSleeps = false;
      break;
    }
    const float expectedY = 1.0F + static_cast<float>(b);
    if (std::fabs(t.position.y - expectedY) > 0.15F) {
      heightsHeld = false;
    }
    const float energy = engine::math::length_sq(body->velocity) +
                         engine::math::length_sq(body->angularVelocity);
    if (energy > 1.0e-6F) {
      residualConverged = false;
    }
    if (!body->sleeping) {
      stackSleeps = false;
    }
  }
  check(heightsHeld, "Stacked boxes hold their heights after 3 seconds");
  check(positionsSteady, "Stack positions stay jitter-free (sub-mm) for 0.5 s");
  check(residualConverged,
        "Residual velocity converges to rest instead of a bounded nonzero "
        "value (issue #123)");
  check(stackSleeps,
        "Resting stack actually sleeps instead of holding forever above "
        "the threshold (issue #123)");
}

/// Removing the resting box's collider must evict its manifold on the next
/// resolve: warm starts can never replay a contact that no longer exists.
static void test_stale_manifold_evicted() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();

  add_floor(*world);
  const engine::core::Entity box = add_box(*world, 0.99F);

  for (int i = 0; i < 5; ++i) {
    run_step(*world);
  }
  const engine::physics::PhysicsContext &ctx = world->physics_context();
  if (engine::physics::manifold_count(ctx) == 0U) {
    check(false, "Eviction test precondition: cache populated");
    return;
  }

  if (!world->remove_collider(box)) {
    check(false, "Collider removal failed");
    return;
  }
  run_step(*world);

  check(engine::physics::manifold_count(ctx) == 0U,
        "Vanished pair's manifold is evicted");
}

/// A destroyed box whose entity index is reused must not hand its manifold
/// to the new entity: the cache keys on index AND generation.
static void test_generation_reuse_cannot_inherit() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();

  add_floor(*world);
  const engine::core::Entity oldBox = add_box(*world, 0.99F);

  for (int i = 0; i < 5; ++i) {
    run_step(*world);
  }

  if (!world->destroy_entity(oldBox)) {
    check(false, "Entity destruction failed");
    return;
  }
  const engine::core::Entity newBox = add_box(*world, 0.99F);
  if (newBox.index != oldBox.index) {
    // Index reuse is the scenario under test; a different index would make
    // the assertion vacuous, so surface it loudly instead of passing.
    check(false, "Entity index was not reused as expected");
    return;
  }
  check(newBox.generation != oldBox.generation,
        "Reused index carries a new generation");

  for (int i = 0; i < 5; ++i) {
    run_step(*world);
  }

  const engine::physics::PhysicsContext &ctx = world->physics_context();
  bool staleReference = false;
  bool freshManifold = false;
  for (std::size_t m = 0U; m < engine::physics::manifold_count(ctx); ++m) {
    const engine::physics::ContactManifold *manifold =
        engine::physics::manifold_get(ctx, m);
    if (manifold == nullptr) {
      continue;
    }
    if ((manifold->entityA == oldBox) || (manifold->entityB == oldBox)) {
      staleReference = true;
    }
    if ((manifold->entityA == newBox) || (manifold->entityB == newBox)) {
      freshManifold = true;
    }
  }
  check(!staleReference, "No manifold references the destroyed generation");
  check(freshManifold, "Reused index resolves through a fresh manifold");
}

/// Warm-started resolution stays bit-deterministic across identical runs.
static void test_warmstart_deterministic() noexcept {
  float positions[2] = {};
  float velocities[2] = {};
  for (int run = 0; run < 2; ++run) {
    auto world = std::unique_ptr<engine::runtime::World>(
        new (std::nothrow) engine::runtime::World());
    if (world == nullptr) {
      check(false, "World allocation failed");
      return;
    }
    world->end_frame_phase();
    add_floor(*world);
    const engine::core::Entity box = add_box(*world, 1.2F);
    for (int i = 0; i < 60; ++i) {
      run_step(*world);
    }
    engine::runtime::Transform t{};
    const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(box);
    if (!world->get_transform(box, &t) || (body == nullptr)) {
      check(false, "Body lookup failed");
      return;
    }
    positions[run] = t.position.y;
    velocities[run] = body->velocity.y;
  }
  check((positions[0] == positions[1]) && (velocities[0] == velocities[1]),
        "Warm-started resolution is bit-deterministic");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== Manifold Warm-Start Tests (issue #110) ===\n");

  test_production_populates_manifold_cache();
  test_stack_settles_without_jitter();
  test_stale_manifold_evicted();
  test_generation_reuse_cannot_inherit();
  test_warmstart_deterministic();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
