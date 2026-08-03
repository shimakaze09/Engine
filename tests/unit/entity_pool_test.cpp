// Verifies entity pool test behavior for the Engine test suite.

#include "engine/runtime/entity_pool.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <memory>
#include <new>

namespace {

using namespace engine::runtime;

bool test_pool_init() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world) {
    std::fprintf(stderr, "FAIL: could not allocate World\n");
    return false;
  }
  EntityPool pool;

  if (!pool.init(world.get(), 10U)) {
    std::fprintf(stderr, "FAIL: pool init\n");
    return false;
  }
  if (!pool.initialised()) {
    std::fprintf(stderr, "FAIL: pool not marked initialised\n");
    return false;
  }
  if (pool.capacity() != 10U) {
    std::fprintf(stderr, "FAIL: expected capacity 10, got %zu\n",
                 pool.capacity());
    return false;
  }
  if (pool.available() != 10U) {
    std::fprintf(stderr, "FAIL: expected available 10, got %zu\n",
                 pool.available());
    return false;
  }
  return true;
}

bool test_pool_acquire_release() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  pool.init(world.get(), 5U);

  Entity entities[5]{};
  for (int i = 0; i < 5; ++i) {
    entities[i] = pool.acquire();
    if (entities[i] == kInvalidEntity) {
      std::fprintf(stderr, "FAIL: acquire %d returned invalid\n", i);
      return false;
    }
  }

  if (pool.available() != 0U) {
    std::fprintf(stderr, "FAIL: expected 0 available after 5 acquires\n");
    return false;
  }

  // Exhausted pool returns invalid.
  if (pool.acquire() != kInvalidEntity) {
    std::fprintf(stderr, "FAIL: expected invalid from exhausted pool\n");
    return false;
  }

  for (auto &entity : entities) {
    if (!pool.release(entity)) {
      std::fprintf(stderr, "FAIL: release failed\n");
      return false;
    }
  }

  if (pool.available() != 5U) {
    std::fprintf(stderr, "FAIL: expected 5 available after release\n");
    return false;
  }

  return true;
}

bool test_pool_handle_reuse() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  constexpr std::size_t kCount = 100U;
  pool.init(world.get(), kCount);

  // Acquire 100 entities and record their indices.
  std::uint32_t firstRoundIndices[kCount]{};
  for (std::size_t i = 0U; i < kCount; ++i) {
    const Entity e = pool.acquire();
    if (e == kInvalidEntity) {
      std::fprintf(stderr, "FAIL: first acquire %zu failed\n", i);
      return false;
    }
    firstRoundIndices[i] = e.index;
  }

  // Release all 100.
  for (std::size_t i = 0U; i < kCount; ++i) {
    const Entity e = world->find_entity_by_index(firstRoundIndices[i]);
    if (!pool.release(e)) {
      std::fprintf(stderr, "FAIL: release %zu failed\n", i);
      return false;
    }
  }

  // Re-acquire 100 — all should reuse the same indices (no new entity IDs).
  std::uint32_t secondRoundIndices[kCount]{};
  for (std::size_t i = 0U; i < kCount; ++i) {
    const Entity e = pool.acquire();
    if (e == kInvalidEntity) {
      std::fprintf(stderr, "FAIL: second acquire %zu failed\n", i);
      return false;
    }
    secondRoundIndices[i] = e.index;
  }

  for (std::size_t i = 0U; i < kCount; ++i) {
    bool found = false;
    for (std::size_t j = 0U; j < kCount; ++j) {
      if (secondRoundIndices[i] == firstRoundIndices[j]) {
        found = true;
        break;
      }
    }
    if (!found) {
      std::fprintf(stderr,
                   "FAIL: second-round index %u not in first-round set\n",
                   secondRoundIndices[i]);
      return false;
    }
  }

  return true;
}

bool test_pool_double_init() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  pool.init(world.get(), 5U);

  if (pool.init(world.get(), 5U)) {
    std::fprintf(stderr, "FAIL: double init should fail\n");
    return false;
  }
  return true;
}

bool test_pool_release_unknown() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  pool.init(world.get(), 2U);

  const Entity outsider = world->create_entity();
  if (pool.release(outsider)) {
    std::fprintf(stderr, "FAIL: releasing non-pool entity should fail\n");
    return false;
  }
  return true;
}

// A stale handle sharing a pooled entity's index but not its generation
// must not free the slot (that would allow double-acquisition).
bool test_pool_release_stale_generation() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  pool.init(world.get(), 2U);

  const Entity acquired = pool.acquire();
  if (acquired == kInvalidEntity) {
    return false;
  }

  Entity stale = acquired;
  stale.generation = acquired.generation + 1U;
  if (pool.release(stale)) {
    std::fprintf(stderr, "FAIL: stale-generation release should fail\n");
    return false;
  }
  if (pool.available() != 1U) {
    std::fprintf(stderr, "FAIL: slot freed by stale handle\n");
    return false;
  }
  return pool.release(acquired) && (pool.available() == 2U);
}

/// EXPECTATION: releasing an entity carrying every component type leaves
/// nothing behind for the next acquire — the pool must go through the
/// World's exhaustive teardown, not a partial removal list (audit H-02).
bool test_pool_release_removes_every_component() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world) {
    return false;
  }
  EntityPool pool;
  if (!pool.init(world.get(), 2U)) {
    return false;
  }

  const Entity entity = pool.acquire();
  if (entity == kInvalidEntity) {
    return false;
  }

  Transform transform{};
  NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "%s", "Pooled");
  AnimationComponent animation{};
  std::snprintf(animation.controllerPath, sizeof(animation.controllerPath),
                "%s", "assets/character.animctrl.json");
  if (!world->add_transform(entity, transform) ||
      !world->add_rigid_body(entity, RigidBody{}) ||
      !world->add_collider(entity, Collider{}) ||
      !world->add_mesh_component(entity, MeshComponent{}) ||
      !world->add_name_component(entity, name) ||
      !world->add_script_component(entity, ScriptComponent{}) ||
      !world->add_light_component(entity, LightComponent{}) ||
      !world->add_point_light_component(entity, PointLightComponent{}) ||
      !world->add_spot_light_component(entity, SpotLightComponent{}) ||
      !world->add_reflection_probe_component(entity,
                                             ReflectionProbeComponent{}) ||
      !world->add_scene_capture_component(entity, SceneCaptureComponent{}) ||
      !world->add_foliage_patch_component(entity, FoliagePatchComponent{}) ||
      !world->add_spring_arm(entity, SpringArmComponent{}) ||
      !world->add_animation_component(entity, animation)) {
    return false;
  }

  if (!pool.release(entity)) {
    return false;
  }
  const Entity reused = pool.acquire();
  if (reused == kInvalidEntity) {
    return false;
  }

  Transform outTransform{};
  RigidBody outBody{};
  Collider outCollider{};
  MeshComponent outMesh{};
  NameComponent outName{};
  ScriptComponent outScript{};
  LightComponent outLight{};
  PointLightComponent outPointLight{};
  SpotLightComponent outSpotLight{};
  ReflectionProbeComponent outProbe{};
  SceneCaptureComponent outCapture{};
  FoliagePatchComponent outFoliage{};
  SpringArmComponent outSpringArm{};
  AnimationComponent outAnimation{};
  if (world->get_transform(reused, &outTransform) ||
      world->get_rigid_body(reused, &outBody) ||
      world->get_collider(reused, &outCollider) ||
      world->get_mesh_component(reused, &outMesh) ||
      world->get_name_component(reused, &outName) ||
      world->get_script_component(reused, &outScript) ||
      world->get_light_component(reused, &outLight) ||
      world->get_point_light_component(reused, &outPointLight) ||
      world->get_spot_light_component(reused, &outSpotLight) ||
      world->get_reflection_probe_component(reused, &outProbe) ||
      world->get_scene_capture_component(reused, &outCapture) ||
      world->get_foliage_patch_component(reused, &outFoliage) ||
      world->get_spring_arm(reused, &outSpringArm) ||
      world->get_animation_component(reused, &outAnimation)) {
    std::fprintf(stderr, "FAIL: reused pool entity inherited a component\n");
    return false;
  }
  return true;
}

/// EXPECTATION: releasing during Simulation is refused without publishing
/// the slot or touching components, and succeeds once a mutation phase
/// returns (audit H-02: cleanup is phase-sensitive and must never be
/// half-applied).
bool test_pool_release_refused_mid_simulation() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world) {
    return false;
  }
  EntityPool pool;
  if (!pool.init(world.get(), 2U)) {
    return false;
  }

  const Entity entity = pool.acquire();
  if (entity == kInvalidEntity) {
    return false;
  }
  NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "%s", "MidSim");
  if (!world->add_transform(entity, Transform{}) ||
      !world->add_name_component(entity, name)) {
    return false;
  }
  const std::size_t availableBefore = pool.available();

  world->begin_update_phase();
  if (pool.release(entity)) {
    world->end_frame_phase();
    std::fprintf(stderr, "FAIL: release succeeded mid-simulation\n");
    return false;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->begin_render_phase();
  world->end_frame_phase();

  NameComponent survivingName{};
  if ((pool.available() != availableBefore) ||
      !world->get_name_component(entity, &survivingName)) {
    std::fprintf(stderr, "FAIL: refused release mutated state\n");
    return false;
  }

  if (!pool.release(entity) || (pool.available() != availableBefore + 1U)) {
    std::fprintf(stderr, "FAIL: release after simulation phase failed\n");
    return false;
  }
  NameComponent removedName{};
  if (world->get_name_component(entity, &removedName)) {
    std::fprintf(stderr, "FAIL: released entity kept its name\n");
    return false;
  }
  return true;
}

} // namespace

/// Runs this executable or test program.
/// Runs one production-style BeginPlay dispatch pass: every entity still
/// needing begin_play is marked done, as the scripting dispatcher does.
void run_begin_play_pass(World *world) {
  world->begin_begin_play_phase();
  world->for_each_needs_begin_play(
      [&](Entity entity) noexcept { world->mark_begin_play_done(entity); });
  world->end_begin_play_phase();
}

/// EXPECTATION (review item 5): dormant pool entities keep BeginPlay
/// consumed so the per-frame dispatch skips them, and acquisition
/// re-arms it so components attached afterwards get fresh-entity
/// callbacks — including after a release/re-acquire cycle.
bool test_pool_acquire_rearms_begin_play() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  EntityPool pool;
  if (!world || !pool.init(world.get(), 2U)) {
    std::fprintf(stderr, "FAIL: setup\n");
    return false;
  }

  run_begin_play_pass(world.get());

  const Entity entity = pool.acquire();
  if (entity == kInvalidEntity) {
    std::fprintf(stderr, "FAIL: acquire\n");
    return false;
  }
  if (world->has_begun_play(entity)) {
    std::fprintf(stderr, "FAIL: acquire did not re-arm BeginPlay\n");
    return false;
  }

  run_begin_play_pass(world.get());
  if (!world->has_begun_play(entity)) {
    std::fprintf(stderr, "FAIL: dispatch did not fire re-armed BeginPlay\n");
    return false;
  }

  if (!pool.release(entity)) {
    std::fprintf(stderr, "FAIL: release\n");
    return false;
  }
  const Entity again = pool.acquire();
  if ((again == kInvalidEntity) || world->has_begun_play(again)) {
    std::fprintf(stderr, "FAIL: re-acquire did not re-arm BeginPlay\n");
    return false;
  }
  return true;
}

/// EXPECTATION (review item 5): releasing a pooled parent destroys its
/// attached children (children never survive their parent's teardown)
/// while the pooled root itself stays alive dormant.
bool test_pool_release_destroys_children() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  EntityPool pool;
  if (!world || !pool.init(world.get(), 2U)) {
    std::fprintf(stderr, "FAIL: setup\n");
    return false;
  }

  const Entity parent = pool.acquire();
  if ((parent == kInvalidEntity) ||
      !world->add_transform(parent, Transform{})) {
    std::fprintf(stderr, "FAIL: parent setup\n");
    return false;
  }
  const Entity child = world->create_scene_object();
  Transform childLocal{};
  childLocal.parentId = world->persistent_id(parent);
  if ((child == kInvalidEntity) ||
      !world->add_transform(child, childLocal)) {
    std::fprintf(stderr, "FAIL: child setup\n");
    return false;
  }

  if (!pool.release(parent)) {
    std::fprintf(stderr, "FAIL: release with child refused\n");
    return false;
  }
  if (world->is_alive(child)) {
    std::fprintf(stderr, "FAIL: child survived the parent's recycle\n");
    return false;
  }
  if (!world->is_alive(parent)) {
    std::fprintf(stderr, "FAIL: recycled parent died\n");
    return false;
  }
  return true;
}

/// EXPECTATION (review item 5): a pooled entity with a deferred destroy
/// queued cannot be recycled — the release is refused, the slot stays in
/// use, and the flush then destroys the entity without touching a
/// re-published slot.
bool test_pool_release_refused_when_destroy_queued() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  EntityPool pool;
  if (!world || !pool.init(world.get(), 2U)) {
    std::fprintf(stderr, "FAIL: setup\n");
    return false;
  }

  const Entity entity = pool.acquire();
  if ((entity == kInvalidEntity) ||
      !world->add_transform(entity, Transform{})) {
    std::fprintf(stderr, "FAIL: acquire\n");
    return false;
  }

  world->begin_update_phase();
  if (!world->destroy_entity(entity)) {
    std::fprintf(stderr, "FAIL: deferred destroy not queued\n");
    return false;
  }
  world->begin_end_play_phase();
  if (pool.release(entity)) {
    std::fprintf(stderr, "FAIL: release accepted with destroy queued\n");
    return false;
  }
  if (pool.available() != 1U) {
    std::fprintf(stderr, "FAIL: refused release changed the free list\n");
    return false;
  }
  world->end_end_play_phase();
  if (world->is_alive(entity)) {
    std::fprintf(stderr, "FAIL: flush did not destroy the entity\n");
    return false;
  }
  return true;
}

/// EXPECTATION (PR #52 review): a pooled entity whose descendants are
/// queued for deferred destruction cannot be recycled — the release is
/// refused with the free list unchanged so the flush still fires EndPlay
/// for every queued member, and the release succeeds after the flush.
bool test_pool_release_refused_when_descendant_destroy_queued() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  EntityPool pool;
  if (!world || !pool.init(world.get(), 2U)) {
    std::fprintf(stderr, "FAIL: setup\n");
    return false;
  }

  const Entity parent = pool.acquire();
  if ((parent == kInvalidEntity) ||
      !world->add_transform(parent, Transform{})) {
    std::fprintf(stderr, "FAIL: parent setup\n");
    return false;
  }
  const Entity child = world->create_scene_object();
  Transform childLocal{};
  childLocal.parentId = world->persistent_id(parent);
  if ((child == kInvalidEntity) ||
      !world->add_transform(child, childLocal)) {
    std::fprintf(stderr, "FAIL: child setup\n");
    return false;
  }
  const Entity grandchild = world->create_scene_object();
  Transform grandchildLocal{};
  grandchildLocal.parentId = world->persistent_id(child);
  if ((grandchild == kInvalidEntity) ||
      !world->add_transform(grandchild, grandchildLocal)) {
    std::fprintf(stderr, "FAIL: grandchild setup\n");
    return false;
  }

  world->begin_update_phase();
  if (!world->destroy_entity(child)) {
    std::fprintf(stderr, "FAIL: deferred subtree destroy not queued\n");
    return false;
  }
  world->begin_end_play_phase();
  if (pool.release(parent)) {
    std::fprintf(stderr, "FAIL: release accepted with descendants queued\n");
    return false;
  }
  if (pool.available() != 1U) {
    std::fprintf(stderr, "FAIL: refused release changed the free list\n");
    return false;
  }
  if (!world->is_alive(child) || !world->is_alive(grandchild)) {
    std::fprintf(stderr, "FAIL: refused release destroyed queued members\n");
    return false;
  }
  world->end_end_play_phase();
  if (world->is_alive(child) || world->is_alive(grandchild)) {
    std::fprintf(stderr, "FAIL: flush did not destroy the queued subtree\n");
    return false;
  }
  if (!world->is_alive(parent)) {
    std::fprintf(stderr, "FAIL: flush destroyed the pooled parent\n");
    return false;
  }
  if (!pool.release(parent)) {
    std::fprintf(stderr, "FAIL: release refused after the flush\n");
    return false;
  }
  if (pool.available() != 2U) {
    std::fprintf(stderr, "FAIL: post-flush release missed the free list\n");
    return false;
  }
  return true;
}

int main() {
  struct TestCase {
    const char *name;
    bool (*fn)();
  };

  const TestCase tests[] = {
      {"pool_init", test_pool_init},
      {"pool_acquire_release", test_pool_acquire_release},
      {"pool_handle_reuse_100", test_pool_handle_reuse},
      {"pool_double_init", test_pool_double_init},
      {"pool_release_unknown", test_pool_release_unknown},
      {"pool_release_stale_generation", test_pool_release_stale_generation},
      {"pool_release_removes_every_component",
       test_pool_release_removes_every_component},
      {"pool_release_refused_mid_simulation",
       test_pool_release_refused_mid_simulation},
      {"pool_acquire_rearms_begin_play", test_pool_acquire_rearms_begin_play},
      {"pool_release_destroys_children", test_pool_release_destroys_children},
      {"pool_release_refused_when_destroy_queued",
       test_pool_release_refused_when_destroy_queued},
      {"pool_release_refused_when_descendant_destroy_queued",
       test_pool_release_refused_when_descendant_destroy_queued},
  };

  int failures = 0;
  for (const auto &tc : tests) {
    std::printf("  %-40s ", tc.name);
    if (tc.fn()) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  return (failures == 0) ? 0 : 1;
}
