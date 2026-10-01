// Verifies frame-scope collision callback accumulation (issue #103): every
// fixed catch-up step's pairs reach the dispatch in step order (once per
// substep), catch-up and one-step-per-frame execution deliver the same
// sequence, and drop accounting covers the whole rendered frame. Frames
// mirror the production pipeline's per-step phase/step/resolve sequence.
// Pairs that cannot respond -- neither side movable -- report no event.

#include "engine/math/component_types.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics_context.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

constexpr float kFixedDt = 1.0F / 60.0F;
constexpr std::size_t kMaxRecordedPairs = 4096U;

std::size_t g_recordedCount = 0U;
std::size_t g_dispatchCalls = 0U;
engine::runtime::Entity g_recordedPairs[kMaxRecordedPairs * 2U] = {};

/// Dispatch callback: appends every delivered pair in delivery order.
void record_collision_pairs(const engine::runtime::Entity *pairs,
                            std::size_t pairCount) noexcept {
  ++g_dispatchCalls;
  for (std::size_t i = 0U; i < pairCount; ++i) {
    if (g_recordedCount >= kMaxRecordedPairs) {
      return;
    }
    g_recordedPairs[g_recordedCount * 2U] = pairs[i * 2U];
    g_recordedPairs[(g_recordedCount * 2U) + 1U] = pairs[(i * 2U) + 1U];
    ++g_recordedCount;
  }
}

/// Clears the recorder before each scenario.
void reset_recorder() noexcept {
  g_recordedCount = 0U;
  g_dispatchCalls = 0U;
}

/// Reports whether recorded pair `slot` contains exactly the two entities
/// (full identity: index and generation).
[[nodiscard]] bool recorded_pair_is(std::size_t slot,
                                    engine::runtime::Entity first,
                                    engine::runtime::Entity second) noexcept {
  if (slot >= g_recordedCount) {
    return false;
  }
  const engine::runtime::Entity a = g_recordedPairs[slot * 2U];
  const engine::runtime::Entity b = g_recordedPairs[(slot * 2U) + 1U];
  return ((a == first) && (b == second)) || ((a == second) && (b == first));
}

/// Creates a World in its component-mutation phase.
[[nodiscard]] std::unique_ptr<engine::runtime::World> make_world() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world != nullptr) {
    world->end_frame_phase();
  }
  return world;
}

/// Adds a sphere collider at `position`, with a body of `type` unless
/// `type` is null (a plain static collider). Invalid on any refusal.
engine::runtime::Entity add_sphere(engine::runtime::World &world,
                                   const engine::math::Vec3 &position,
                                   const engine::runtime::BodyType *type) {
  engine::runtime::Transform t{};
  t.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(t);
  engine::runtime::Collider col{};
  col.shape = engine::runtime::ColliderShape::Sphere;
  col.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world.add_collider(entity, col)) {
    return engine::runtime::kInvalidEntity;
  }
  if (type != nullptr) {
    engine::runtime::RigidBody body{};
    body.inverseMass = 1.0F;
    body.bodyType = static_cast<std::uint32_t>(*type);
    if (!world.add_rigid_body(entity, body)) {
      return engine::runtime::kInvalidEntity;
    }
  }
  return entity;
}

/// A dynamic sphere resting 0.01 m deep on a static one at `x`, pressed
/// down by gravity so the contact persists from step to step. Returns the
/// two entities, static first. A pair of static colliders would report
/// nothing, since neither can respond.
bool add_resting_pair(engine::runtime::World &world, float x,
                      engine::runtime::Entity *outBase,
                      engine::runtime::Entity *outBall) {
  const engine::runtime::BodyType dynamic = engine::runtime::BodyType::Dynamic;
  *outBase = add_sphere(world, engine::math::Vec3(x, 0.0F, 0.0F), nullptr);
  *outBall = add_sphere(world, engine::math::Vec3(x, 0.99F, 0.0F), &dynamic);
  return (*outBase != engine::runtime::kInvalidEntity) &&
         (*outBall != engine::runtime::kInvalidEntity);
}

/// Runs one rendered frame of `stepCount` fixed steps through the
/// production bridge sequence, without the final callback dispatch.
[[nodiscard]] bool run_frame_steps(engine::runtime::World &world,
                                   std::size_t stepCount) noexcept {
  world.begin_update_phase();
  for (std::size_t step = 0U; step < stepCount; ++step) {
    if (step > 0U) {
      world.begin_update_step();
    }
    if (!world.update_transforms_range(0U, world.transform_count(),
                                       kFixedDt) ||
        !engine::runtime::step_physics(world, kFixedDt) ||
        !engine::runtime::resolve_collisions(world, kFixedDt)) {
      world.end_frame_phase();
      return false;
    }
    world.commit_update_phase();
  }
  world.begin_render_prep_phase();
  world.end_frame_phase();
  return true;
}

/// Builds the transient-contact world: a slow upward sphere that overlaps
/// the static block only during the frame's first fixed step.
[[nodiscard]] bool
build_transient_world(engine::runtime::World &world,
                      engine::runtime::Entity *outBlock,
                      engine::runtime::Entity *outSphere) noexcept {
  engine::runtime::set_gravity(world, 0.0F, 0.0F, 0.0F);
  engine::runtime::set_collision_dispatch(world, &record_collision_pairs);

  engine::runtime::Transform blockTransform{};
  const engine::runtime::Entity block =
      world.create_scene_object(blockTransform);

  engine::runtime::Transform sphereTransform{};
  sphereTransform.position = engine::math::Vec3(0.0F, 0.96F, 0.0F);
  const engine::runtime::Entity sphere =
      world.create_scene_object(sphereTransform);
  if ((block == engine::runtime::kInvalidEntity) ||
      (sphere == engine::runtime::kInvalidEntity)) {
    return false;
  }

  engine::runtime::Collider blockCollider{};
  blockCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::Collider sphereCollider{};
  sphereCollider.shape = engine::runtime::ColliderShape::Sphere;
  sphereCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  // 1.8 m/s upward: 0.03 m per step, entering 0.01 m of overlap on step
  // one and at least 0.02 m of clearance from step two onward; slow enough
  // to stay below the CCD engagement threshold.
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  body.velocity = engine::math::Vec3(0.0F, 1.8F, 0.0F);

  if (!world.add_collider(block, blockCollider) ||
      !world.add_collider(sphere, sphereCollider) ||
      !world.add_rigid_body(sphere, body)) {
    return false;
  }

  *outBlock = block;
  *outSphere = sphere;
  return true;
}

/// Core regression: a contact existing only in the first of two catch-up
/// steps must still reach the dispatch at frame end.
int check_first_step_contact_survives_catchup() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return 10;
  }

  engine::runtime::Entity block{};
  engine::runtime::Entity sphere{};
  if (!build_transient_world(*world, &block, &sphere)) {
    return 11;
  }

  reset_recorder();
  if (!run_frame_steps(*world, 2U)) {
    return 12;
  }
  engine::runtime::dispatch_collision_callbacks(*world);

  if (g_recordedCount != 1U) {
    std::fprintf(stderr, "transient contact: expected 1 event, got %zu\n",
                 g_recordedCount);
    return 13;
  }
  if (!recorded_pair_is(0U, block, sphere)) {
    return 14;
  }
  return 0;
}

/// Once-per-substep policy: a pair persisting across three catch-up steps
/// is delivered three times, and a later contact-free frame delivers
/// nothing (the frame buffer resets on dispatch).
int check_once_per_substep_and_empty_frame() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return 20;
  }

  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);

  engine::runtime::Entity a{};
  engine::runtime::Entity b{};
  if (!add_resting_pair(*world, 0.0F, &a, &b)) {
    return 21;
  }

  reset_recorder();
  if (!run_frame_steps(*world, 3U)) {
    return 23;
  }
  engine::runtime::dispatch_collision_callbacks(*world);

  if ((g_recordedCount != 3U) || (g_dispatchCalls != 1U)) {
    std::fprintf(stderr, "substep policy: expected 3 events, got %zu\n",
                 g_recordedCount);
    return 24;
  }
  for (std::size_t i = 0U; i < 3U; ++i) {
    if (!recorded_pair_is(i, a, b)) {
      return 25;
    }
  }

  // Zero-collision boundary: a resting contact does not end on its own, so
  // the pair-free follow-up removes one collider before the next frame.
  if (!world->remove_collider(b)) {
    return 26;
  }
  reset_recorder();
  if (!run_frame_steps(*world, 2U)) {
    return 27;
  }
  engine::runtime::dispatch_collision_callbacks(*world);
  if ((g_recordedCount != 0U) || (g_dispatchCalls != 0U)) {
    return 28;
  }
  return 0;
}

/// Determinism: one-step-per-frame execution and a two-step catch-up frame
/// deliver identical event sequences for identical worlds.
int check_catchup_matches_single_step_frames() noexcept {
  engine::runtime::Entity singleStepPairs[4U] = {};
  std::size_t singleStepCount = 0U;

  {
    std::unique_ptr<engine::runtime::World> world = make_world();
    engine::runtime::Entity block{};
    engine::runtime::Entity sphere{};
    if ((world == nullptr) ||
        !build_transient_world(*world, &block, &sphere)) {
      return 30;
    }

    reset_recorder();
    for (std::size_t frame = 0U; frame < 2U; ++frame) {
      if (!run_frame_steps(*world, 1U)) {
        return 31;
      }
      engine::runtime::dispatch_collision_callbacks(*world);
    }
    if (g_recordedCount > 2U) {
      return 32;
    }
    singleStepCount = g_recordedCount;
    for (std::size_t i = 0U; i < (singleStepCount * 2U); ++i) {
      singleStepPairs[i] = g_recordedPairs[i];
    }
  }

  {
    std::unique_ptr<engine::runtime::World> world = make_world();
    engine::runtime::Entity block{};
    engine::runtime::Entity sphere{};
    if ((world == nullptr) ||
        !build_transient_world(*world, &block, &sphere)) {
      return 33;
    }

    reset_recorder();
    if (!run_frame_steps(*world, 2U)) {
      return 34;
    }
    engine::runtime::dispatch_collision_callbacks(*world);
  }

  if (g_recordedCount != singleStepCount) {
    std::fprintf(stderr, "determinism: catch-up %zu events vs %zu\n",
                 g_recordedCount, singleStepCount);
    return 35;
  }
  for (std::size_t i = 0U; i < (singleStepCount * 2U); ++i) {
    if (g_recordedPairs[i] != singleStepPairs[i]) {
      return 36;
    }
  }
  return 0;
}

/// Whole-frame overflow accounting (H-08 continuity): two capacity-busting
/// steps keep both steps' capped sets, count every drop for the frame, and
/// stay one loud episode; dispatch resets the frame counters.
int check_frame_overflow_accounting() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return 40;
  }

  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);

  // 1030 well-separated resting pairs: 6 drops per step at the 1024-pair
  // step cap (the H-08 workload).
  constexpr int kPairCount = 1030;
  for (int p = 0; p < kPairCount; ++p) {
    engine::runtime::Entity base{};
    engine::runtime::Entity ball{};
    if (!add_resting_pair(*world, static_cast<float>(p) * 10.0F, &base,
                          &ball)) {
      return 47;
    }
  }

  reset_recorder();
  if (!run_frame_steps(*world, 2U)) {
    return 41;
  }

  const engine::physics::PhysicsContext &ctx = world->physics_context();
  if (ctx.frameCollisionPairCount !=
      (2U * engine::physics::kMaxCollisionPairs)) {
    return 42;
  }
  if (ctx.frameCollisionPairDropCount != 12U) {
    std::fprintf(stderr, "overflow: expected 12 frame drops, got %u\n",
                 ctx.frameCollisionPairDropCount);
    return 43;
  }
  if (ctx.collisionPairOverflowEpisodes != 1U) {
    return 44;
  }

  engine::runtime::dispatch_collision_callbacks(*world);
  if (g_recordedCount != (2U * engine::physics::kMaxCollisionPairs)) {
    std::fprintf(stderr, "overflow: expected 2048 events, got %zu\n",
                 g_recordedCount);
    return 45;
  }
  if ((ctx.frameCollisionPairCount != 0U) ||
      (ctx.frameCollisionPairDropCount != 0U)) {
    return 46;
  }
  return 0;
}

/// Two colliders that cannot move -- static against static, kinematic
/// against static -- have no response and report no event, as in Unity,
/// Jolt and Box2D. A blocked-out level overlapping 1,100 static pairs used
/// to be re-recorded every step: the pair cap filled with them, the one
/// dynamic contact past it was dropped, and Lua was called for each.
int check_immovable_pairs_are_not_reported() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return 50;
  }
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);
  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);

  constexpr int kStaticPairs = 1100;
  for (int p = 0; p < kStaticPairs; ++p) {
    const float x = static_cast<float>(p) * 10.0F;
    if ((add_sphere(*world, engine::math::Vec3(x, 0.0F, 0.0F), nullptr) ==
         engine::runtime::kInvalidEntity) ||
        (add_sphere(*world, engine::math::Vec3(x + 0.5F, 0.0F, 0.0F),
                    nullptr) == engine::runtime::kInvalidEntity)) {
      return 51;
    }
  }
  // A kinematic sphere at rest inside a static one, and last in traversal
  // order, a dynamic sphere sunk into a static one: the only pair that can
  // respond.
  const engine::runtime::BodyType kinematic =
      engine::runtime::BodyType::Kinematic;
  const engine::runtime::BodyType dynamic = engine::runtime::BodyType::Dynamic;
  const engine::math::Vec3 kinematicAt(0.0F, 100.0F, 0.0F);
  const engine::math::Vec3 dynamicAt(0.0F, 200.0F, 0.0F);
  const engine::runtime::Entity floor = add_sphere(*world, dynamicAt, nullptr);
  if ((add_sphere(*world, kinematicAt, nullptr) ==
       engine::runtime::kInvalidEntity) ||
      (add_sphere(
           *world,
           engine::math::add(kinematicAt, engine::math::Vec3(0.5F, 0.0F, 0.0F)),
           &kinematic) == engine::runtime::kInvalidEntity) ||
      (floor == engine::runtime::kInvalidEntity)) {
    return 52;
  }
  const engine::runtime::Entity ball = add_sphere(
      *world,
      engine::math::add(dynamicAt, engine::math::Vec3(0.5F, 0.0F, 0.0F)),
      &dynamic);
  if (ball == engine::runtime::kInvalidEntity) {
    return 53;
  }

  reset_recorder();
  if (!run_frame_steps(*world, 1U)) {
    return 54;
  }
  const engine::physics::PhysicsContext &ctx = world->physics_context();
  if ((ctx.narrowPhasePairTests != 1U) ||
      (ctx.immovablePairsSkipped != (kStaticPairs + 1U))) {
    std::fprintf(stderr,
                 "immovable pairs: %u narrow-phase tests (1 expected), %u "
                 "skipped (%d expected)\n",
                 ctx.narrowPhasePairTests, ctx.immovablePairsSkipped,
                 kStaticPairs + 1);
    return 55;
  }
  if (ctx.frameCollisionPairDropCount != 0U) {
    std::fprintf(stderr, "immovable pairs: %u pairs dropped\n",
                 ctx.frameCollisionPairDropCount);
    return 56;
  }
  engine::runtime::dispatch_collision_callbacks(*world);
  if ((g_recordedCount != 1U) || !recorded_pair_is(0U, floor, ball)) {
    std::fprintf(stderr,
                 "immovable pairs: %zu events delivered; only the dynamic "
                 "contact should be\n",
                 g_recordedCount);
    return 57;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_first_step_contact_survives_catchup();
  if (result != 0) {
    return result;
  }

  result = check_once_per_substep_and_empty_frame();
  if (result != 0) {
    return result;
  }

  result = check_catchup_matches_single_step_frames();
  if (result != 0) {
    return result;
  }

  result = check_frame_overflow_accounting();
  if (result != 0) {
    return result;
  }

  result = check_immovable_pairs_are_not_reported();
  if (result != 0) {
    return result;
  }

  return 0;
}
