// Verifies trigger colliders through the production step/resolve/dispatch
// sequence: a body passes through a trigger with one begin and one end
// event and no contact; which pairs report (a movable side, not two
// triggers, the layer mask, not a body's own colliders); a sleeping body
// stays inside; destroying or disabling reports the end; catch-up frames,
// storage order and capacity overflow never change or invent events; and
// queries, CCD and mass properties ignore triggers.

#include "engine/math/component_types.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics_context.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using engine::math::Vec3;
using engine::runtime::BodyType;
using engine::runtime::Collider;
using engine::runtime::ColliderShape;
using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::RigidBody;
using engine::runtime::Transform;
using engine::runtime::World;

constexpr float kFixedDt = 1.0F / 60.0F;
constexpr std::size_t kMaxRecorded = 4096U;

/// One delivered trigger event and the frame it arrived in.
struct RecordedEvent final {
  Entity trigger{};
  Entity other{};
  bool entered = false;
  std::size_t frame = 0U;
};

RecordedEvent g_events[kMaxRecorded] = {};
std::size_t g_eventCount = 0U;
std::size_t g_currentFrame = 0U;
std::size_t g_collisionPairs = 0U;

/// Trigger dispatch: appends every event in delivery order.
void record_trigger_events(const Entity *pairs, const std::uint8_t *entered,
                           std::size_t count) noexcept {
  for (std::size_t i = 0U; (i < count) && (g_eventCount < kMaxRecorded); ++i) {
    RecordedEvent &event = g_events[g_eventCount];
    event.trigger = pairs[i * 2U];
    event.other = pairs[(i * 2U) + 1U];
    event.entered = entered[i] != 0U;
    event.frame = g_currentFrame;
    ++g_eventCount;
  }
}

/// Collision dispatch: counts pairs, which a trigger must never produce.
void count_collision_pairs(const Entity *, std::size_t count) noexcept {
  g_collisionPairs += count;
}

void reset_recorder() noexcept {
  g_eventCount = 0U;
  g_currentFrame = 0U;
  g_collisionPairs = 0U;
}

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

[[nodiscard]] bool event_is(std::size_t slot, Entity trigger, Entity other,
                            bool entered) noexcept {
  return (slot < g_eventCount) && (g_events[slot].trigger == trigger) &&
         (g_events[slot].other == other) && (g_events[slot].entered == entered);
}

/// A World in its component-mutation phase with both dispatches installed
/// and no gravity.
[[nodiscard]] std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
    engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);
    engine::runtime::set_collision_dispatch(*world, &count_collision_pairs);
    engine::runtime::set_trigger_dispatch(*world, &record_trigger_events);
  }
  return world;
}

/// Adds a collider entity. `type` null means no body (a plain static
/// collider); otherwise a body of that type moving at `velocity`.
Entity add_collider_entity(World &world, const Vec3 &position,
                           ColliderShape shape, const Vec3 &halfExtents,
                           bool isTrigger, const BodyType *type,
                           const Vec3 &velocity = Vec3(0.0F, 0.0F, 0.0F)) {
  Transform transform{};
  transform.position = position;
  const Entity entity = world.create_scene_object(transform);
  Collider collider{};
  collider.shape = shape;
  collider.halfExtents = halfExtents;
  collider.isTrigger = isTrigger;
  if ((entity == kInvalidEntity) || !world.add_collider(entity, collider)) {
    return kInvalidEntity;
  }
  if (type != nullptr) {
    RigidBody body{};
    body.inverseMass = 1.0F;
    body.bodyType = static_cast<std::uint32_t>(*type);
    body.velocity = velocity;
    if (!world.add_rigid_body(entity, body)) {
      return kInvalidEntity;
    }
  }
  return entity;
}

Entity add_trigger_box(World &world, const Vec3 &position,
                       const Vec3 &halfExtents) {
  return add_collider_entity(world, position, ColliderShape::AABB, halfExtents,
                             true, nullptr);
}

Entity add_ball(World &world, const Vec3 &position, BodyType type,
                const Vec3 &velocity = Vec3(0.0F, 0.0F, 0.0F)) {
  return add_collider_entity(world, position, ColliderShape::Sphere,
                             Vec3(0.5F, 0.5F, 0.5F), false, &type, velocity);
}

/// Runs one rendered frame of `stepCount` fixed steps through the
/// production bridge sequence, then the frame-end dispatch.
[[nodiscard]] bool run_frame(World &world, std::size_t stepCount) noexcept {
  world.begin_update_phase();
  for (std::size_t step = 0U; step < stepCount; ++step) {
    if (step > 0U) {
      world.begin_update_step();
    }
    if (!world.update_transforms_range(0U, world.transform_count(), kFixedDt) ||
        !engine::runtime::step_physics(world, kFixedDt) ||
        !engine::runtime::resolve_collisions(world, kFixedDt)) {
      world.end_frame_phase();
      return false;
    }
    world.commit_update_phase();
  }
  world.begin_render_prep_phase();
  world.end_frame_phase();
  engine::runtime::dispatch_collision_callbacks(world);
  ++g_currentFrame;
  return true;
}

[[nodiscard]] bool run_frames(World &world, std::size_t frames,
                              std::size_t stepsPerFrame = 1U) noexcept {
  for (std::size_t i = 0U; i < frames; ++i) {
    if (!run_frame(world, stepsPerFrame)) {
      return false;
    }
  }
  return true;
}

/// The pass-through scene: a static trigger box 2 m wide at the origin
/// and a ball of radius 0.5 crossing it at 6 m/s (0.1 m per step, below
/// the CCD engagement threshold). Its center starts at x = -3.05, so it
/// first overlaps after step 16 (center -1.45) and last after step 45
/// (center 1.45), 0.05 m from either boundary.
struct PassThroughScene final {
  Entity trigger{};
  Entity ball{};
};

[[nodiscard]] bool build_pass_through(World &world, bool isTrigger,
                                      PassThroughScene *out) {
  out->trigger =
      add_collider_entity(world, Vec3(0.0F, 0.0F, 0.0F), ColliderShape::AABB,
                          Vec3(1.0F, 1.0F, 1.0F), isTrigger, nullptr);
  out->ball = add_ball(world, Vec3(-3.05F, 0.0F, 0.0F), BodyType::Dynamic,
                       Vec3(6.0F, 0.0F, 0.0F));
  return (out->trigger != kInvalidEntity) && (out->ball != kInvalidEntity);
}

void check_body_passes_through_with_enter_and_exit() {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  PassThroughScene scene{};
  check((world != nullptr) && build_pass_through(*world, true, &scene),
        "pass-through: scene builds");
  if (g_failures != 0) {
    return;
  }
  check(run_frames(*world, 80U), "pass-through: 80 frames run");
  check(g_eventCount == 2U, "pass-through: exactly one enter and one exit");
  check(event_is(0U, scene.trigger, scene.ball, true),
        "pass-through: first event is enter(trigger, ball)");
  check(event_is(1U, scene.trigger, scene.ball, false),
        "pass-through: second event is exit(trigger, ball)");
  check((g_eventCount >= 1U) && (g_events[0].frame == 15U),
        "pass-through: enter arrives after step 16 (frame index 15)");
  check((g_eventCount >= 2U) && (g_events[1].frame == 45U),
        "pass-through: exit arrives after step 46 (frame index 45)");
  check(g_collisionPairs == 0U, "pass-through: no collision event");

  RigidBody body{};
  check(world->get_rigid_body(scene.ball, &body) && (body.velocity.x == 6.0F) &&
            (body.velocity.y == 0.0F) && (body.velocity.z == 0.0F),
        "pass-through: the trigger leaves the ball's velocity unchanged");

  // Control: the same box without the flag stops the ball.
  reset_recorder();
  std::unique_ptr<World> solid = make_world();
  PassThroughScene solidScene{};
  check((solid != nullptr) && build_pass_through(*solid, false, &solidScene) &&
            run_frames(*solid, 80U),
        "pass-through control: scene runs");
  RigidBody blocked{};
  check(solid->get_rigid_body(solidScene.ball, &blocked) &&
            (blocked.velocity.x < 6.0F),
        "pass-through control: a solid box changes the ball's velocity");
  check(g_eventCount == 0U, "pass-through control: no trigger events");
}

void check_which_pairs_report() {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "filters: world");
  if (world == nullptr) {
    return;
  }
  const Vec3 big(2.0F, 2.0F, 2.0F);

  // A static collider inside a static trigger: nothing can move.
  const Entity staticTrigger = add_trigger_box(*world, Vec3(0, 0, 0), big);
  const Entity wall =
      add_collider_entity(*world, Vec3(0.5F, 0, 0), ColliderShape::AABB,
                          Vec3(0.5F, 0.5F, 0.5F), false, nullptr);
  // A kinematic body at rest inside a static trigger: it can move.
  const Entity kinematicTrigger = add_trigger_box(*world, Vec3(20, 0, 0), big);
  const Entity kinematic =
      add_ball(*world, Vec3(20.5F, 0, 0), BodyType::Kinematic);
  // A dynamic trigger inside a static trigger: triggers ignore triggers.
  const Entity outerTrigger = add_trigger_box(*world, Vec3(40, 0, 0), big);
  const BodyType dynamic = BodyType::Dynamic;
  const Entity innerTrigger =
      add_collider_entity(*world, Vec3(40.5F, 0, 0), ColliderShape::Sphere,
                          Vec3(0.5F, 0.5F, 0.5F), true, &dynamic);
  // A dynamic ball the trigger's mask excludes.
  Collider masked{};
  masked.halfExtents = big;
  masked.isTrigger = true;
  masked.collisionMask = ~2U;
  Transform maskedAt{};
  maskedAt.position = Vec3(60, 0, 0);
  const Entity maskedTrigger = world->create_scene_object(maskedAt);
  const bool maskedAdded = world->add_collider(maskedTrigger, masked);
  Collider layerTwo{};
  layerTwo.shape = ColliderShape::Sphere;
  layerTwo.collisionLayer = 2U;
  Transform layerTwoAt{};
  layerTwoAt.position = Vec3(60.5F, 0, 0);
  const Entity layerTwoBall = world->create_scene_object(layerTwoAt);
  RigidBody layerTwoBody{};
  layerTwoBody.inverseMass = 1.0F;
  const bool layerTwoAdded = world->add_collider(layerTwoBall, layerTwo) &&
                             world->add_rigid_body(layerTwoBall, layerTwoBody);
  // A body's own trigger child never reports the body's own collider.
  const Entity owner = add_ball(*world, Vec3(80, 0, 0), BodyType::Dynamic);
  Transform childAt{};
  childAt.parentId = world->persistent_id(owner);
  const Entity ownTrigger = world->create_scene_object(childAt);
  Collider ownTriggerCollider{};
  ownTriggerCollider.halfExtents = big;
  ownTriggerCollider.isTrigger = true;
  const bool ownAdded = world->add_collider(ownTrigger, ownTriggerCollider);

  check((staticTrigger != kInvalidEntity) && (wall != kInvalidEntity) &&
            (kinematicTrigger != kInvalidEntity) &&
            (kinematic != kInvalidEntity) && (outerTrigger != kInvalidEntity) &&
            (innerTrigger != kInvalidEntity) && maskedAdded && layerTwoAdded &&
            (owner != kInvalidEntity) && ownAdded,
        "filters: scene builds");
  check(run_frames(*world, 3U), "filters: frames run");
  check(g_eventCount == 1U, "filters: only the kinematic pair reports");
  check(event_is(0U, kinematicTrigger, kinematic, true),
        "filters: the kinematic body entering is reported as enter");
  check(g_collisionPairs == 0U, "filters: no collision events");
}

void check_sleeping_body_stays_inside() {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "sleep: world");
  if (world == nullptr) {
    return;
  }
  engine::runtime::set_gravity(*world, 0.0F, -9.8F, 0.0F);
  const Entity floor =
      add_collider_entity(*world, Vec3(0, -0.5F, 0), ColliderShape::AABB,
                          Vec3(5.0F, 0.5F, 5.0F), false, nullptr);
  const Entity zone =
      add_trigger_box(*world, Vec3(0, 1, 0), Vec3(2.0F, 1.0F, 2.0F));
  const Entity ball = add_ball(*world, Vec3(0, 0.6F, 0), BodyType::Dynamic);
  check((floor != kInvalidEntity) && (zone != kInvalidEntity) &&
            (ball != kInvalidEntity),
        "sleep: scene builds");
  check(run_frames(*world, 240U), "sleep: frames run");
  RigidBody body{};
  check(world->get_rigid_body(ball, &body) && body.sleeping,
        "sleep: the ball has fallen asleep on the floor");
  check(g_eventCount == 1U, "sleep: one enter and no exit while asleep");
  check(event_is(0U, zone, ball, true), "sleep: the event is the enter");
}

void check_destroy_and_disable_report_exit() {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "exit: world");
  if (world == nullptr) {
    return;
  }
  const Vec3 big(2.0F, 2.0F, 2.0F);
  const Entity zoneA = add_trigger_box(*world, Vec3(0, 0, 0), big);
  const Entity ballA = add_ball(*world, Vec3(0.5F, 0, 0), BodyType::Dynamic);
  const Entity zoneB = add_trigger_box(*world, Vec3(20, 0, 0), big);
  const Entity ballB = add_ball(*world, Vec3(20.5F, 0, 0), BodyType::Dynamic);
  check((zoneA != kInvalidEntity) && (ballA != kInvalidEntity) &&
            (zoneB != kInvalidEntity) && (ballB != kInvalidEntity),
        "exit: scene builds");
  check(run_frames(*world, 1U) && (g_eventCount == 2U),
        "exit: both balls enter");

  // Destroying a participant ends its overlap with its recorded identity.
  check(world->destroy_entity(ballA), "exit: ball A destroyed");
  // Clearing the flag ends the overlap too.
  Collider plain{};
  plain.halfExtents = big;
  check(world->add_collider(zoneB, plain), "exit: zone B made solid");
  g_eventCount = 0U;
  check(run_frames(*world, 1U), "exit: frame runs");
  check(g_eventCount == 2U, "exit: two end events");
  check(event_is(0U, zoneA, ballA, false),
        "exit: destroyed ball reported with its recorded identity");
  check(event_is(1U, zoneB, ballB, false),
        "exit: a trigger made solid reports the end");
}

/// Runs the pass-through scene with `stepsPerFrame` steps per frame and
/// copies the delivered (trigger, other, entered) sequence.
std::size_t pass_through_sequence(std::size_t stepsPerFrame,
                                  RecordedEvent *out) {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  PassThroughScene scene{};
  if ((world == nullptr) || !build_pass_through(*world, true, &scene) ||
      !run_frames(*world, 120U / stepsPerFrame, stepsPerFrame)) {
    return 0U;
  }
  for (std::size_t i = 0U; i < g_eventCount; ++i) {
    out[i] = g_events[i];
  }
  return g_eventCount;
}

void check_catchup_frames_deliver_the_same_events() {
  RecordedEvent single[8] = {};
  RecordedEvent catchup[8] = {};
  const std::size_t singleCount = pass_through_sequence(1U, single);
  const std::size_t catchupCount = pass_through_sequence(3U, catchup);
  check((singleCount == 2U) && (catchupCount == 2U),
        "catch-up: both runs deliver two events");
  for (std::size_t i = 0U; (i < singleCount) && (i < catchupCount); ++i) {
    check((single[i].trigger == catchup[i].trigger) &&
              (single[i].other == catchup[i].other) &&
              (single[i].entered == catchup[i].entered),
          "catch-up: same event in the same order");
  }
  // Step 16 falls in the sixth 3-step frame (steps 16-18), step 46 in the
  // sixteenth (steps 46-48).
  check((catchupCount == 2U) && (catchup[0].frame == 5U) &&
            (catchup[1].frame == 15U),
        "catch-up: each event arrives in the frame holding its step");
}

/// Two triggers and two balls entering in the same step, with colliders
/// added in `reverse` order so dense storage differs while entity
/// identities do not. Copies the delivered events.
std::size_t ordering_sequence(bool reverse, RecordedEvent *out) {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 0U;
  }
  Entity entities[4] = {};
  const Vec3 positions[4] = {Vec3(0, 0, 0), Vec3(0.5F, 0, 0), Vec3(20, 0, 0),
                             Vec3(20.5F, 0, 0)};
  for (std::size_t i = 0U; i < 4U; ++i) {
    Transform transform{};
    transform.position = positions[i];
    entities[i] = world->create_scene_object(transform);
  }
  for (std::size_t k = 0U; k < 4U; ++k) {
    const std::size_t i = reverse ? (3U - k) : k;
    Collider collider{};
    const bool trigger = (i % 2U) == 0U;
    collider.isTrigger = trigger;
    collider.shape = trigger ? ColliderShape::AABB : ColliderShape::Sphere;
    collider.halfExtents =
        trigger ? Vec3(2.0F, 2.0F, 2.0F) : Vec3(0.5F, 0.5F, 0.5F);
    if (!world->add_collider(entities[i], collider)) {
      return 0U;
    }
    if (!trigger) {
      RigidBody body{};
      body.inverseMass = 1.0F;
      if (!world->add_rigid_body(entities[i], body)) {
        return 0U;
      }
    }
  }
  if (!run_frames(*world, 1U)) {
    return 0U;
  }
  for (std::size_t i = 0U; i < g_eventCount; ++i) {
    out[i] = g_events[i];
  }
  return g_eventCount;
}

void check_order_follows_identity_not_storage() {
  RecordedEvent forward[8] = {};
  RecordedEvent backward[8] = {};
  const std::size_t forwardCount = ordering_sequence(false, forward);
  const std::size_t backwardCount = ordering_sequence(true, backward);
  check((forwardCount == 2U) && (backwardCount == 2U),
        "order: both worlds report two enters");
  for (std::size_t i = 0U; (i < forwardCount) && (i < backwardCount); ++i) {
    check((forward[i].trigger == backward[i].trigger) &&
              (forward[i].other == backward[i].other) &&
              (forward[i].entered == backward[i].entered),
          "order: storage order does not change the event order");
  }
  check((forwardCount == 2U) &&
            (forward[0].trigger.index < forward[1].trigger.index),
        "order: events follow the trigger's entity index");
}

void check_overflow_keeps_the_previous_set() {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "overflow: world");
  if (world == nullptr) {
    return;
  }
  // One trigger over a grid of kMaxTriggerOverlaps + 1 resting balls,
  // 2 m apart so no two balls touch.
  constexpr std::size_t kBalls = engine::physics::kMaxTriggerOverlaps + 1U;
  constexpr std::size_t kSide = 33U;
  const Entity zone = add_trigger_box(*world, Vec3(32.0F, 0.0F, 32.0F),
                                      Vec3(40.0F, 2.0F, 40.0F));
  bool built = zone != kInvalidEntity;
  Entity last{};
  for (std::size_t i = 0U; built && (i < kBalls); ++i) {
    const float x = static_cast<float>(i % kSide) * 2.0F;
    const float z = static_cast<float>(i / kSide) * 2.0F;
    last = add_ball(*world, Vec3(x, 0.0F, z), BodyType::Dynamic);
    built = last != kInvalidEntity;
  }
  check(built, "overflow: scene builds");
  check(run_frames(*world, 2U), "overflow: frames run");
  const engine::physics::PhysicsContext &ctx = world->physics_context();
  check(g_eventCount == 0U, "overflow: nothing reported while over capacity");
  check(ctx.triggerOverlapOverflowEpisodes == 1U,
        "overflow: one logged episode");
  check(ctx.shapeStore->triggerOverlapCount == 0U,
        "overflow: the previous (empty) set is kept");

  check(world->remove_collider(last), "overflow: one ball removed");
  check(run_frames(*world, 1U), "overflow: frame runs");
  check(g_eventCount == engine::physics::kMaxTriggerOverlaps,
        "overflow: every overlap is reported once the count fits");
  bool allEnters = true;
  for (std::size_t i = 0U; i < g_eventCount; ++i) {
    allEnters =
        allEnters && g_events[i].entered && (g_events[i].trigger == zone);
  }
  check(allEnters, "overflow: the reported events are enters");
  check(!ctx.triggerOverlapOverflowActive, "overflow: the episode ends");
}

void check_queries_and_ccd_ignore_triggers() {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "queries: world");
  if (world == nullptr) {
    return;
  }
  const Entity zone =
      add_trigger_box(*world, Vec3(0, 0, 0), Vec3(1.0F, 1.0F, 1.0F));
  const Entity wall =
      add_collider_entity(*world, Vec3(5, 0, 0), ColliderShape::AABB,
                          Vec3(0.5F, 1.0F, 1.0F), false, nullptr);
  check((zone != kInvalidEntity) && (wall != kInvalidEntity),
        "queries: scene builds");

  engine::runtime::PhysicsRaycastHit hit{};
  check(engine::runtime::raycast(*world, Vec3(-5, 0, 0), Vec3(1, 0, 0), 20.0F,
                                 &hit) &&
            (hit.entity == wall),
        "queries: raycast passes through the trigger to the wall");
  engine::runtime::PhysicsRaycastHit hits[4] = {};
  const std::size_t hitCount = engine::runtime::raycast_all(
      *world, Vec3(-5, 0, 0), Vec3(1, 0, 0), 20.0F, hits, 4U);
  check((hitCount == 1U) && (hits[0].entity == wall),
        "queries: raycast_all reports only the wall");
  std::uint32_t found[4] = {};
  check(engine::runtime::overlap_sphere(*world, Vec3(0, 0, 0), 0.5F, found,
                                        4U) == 0U,
        "queries: overlap_sphere ignores the trigger");
  check(engine::runtime::overlap_box(*world, Vec3(0, 0, 0),
                                     Vec3(0.5F, 0.5F, 0.5F), found, 4U) == 0U,
        "queries: overlap_box ignores the trigger");
  engine::physics::SweepHit sweep{};
  check(engine::runtime::sweep_sphere(*world, Vec3(-5, 0, 0), 0.25F,
                                      Vec3(1, 0, 0), 20.0F, &sweep) &&
            (sweep.entityIndex == wall.index),
        "queries: sweep_sphere passes through the trigger");
  check(engine::runtime::sweep_box(*world, Vec3(-5, 0, 0),
                                   Vec3(0.25F, 0.25F, 0.25F), Vec3(1, 0, 0),
                                   20.0F, &sweep) &&
            (sweep.entityIndex == wall.index),
        "queries: sweep_box passes through the trigger");

  // A ball at 120 m/s (2 m per step, past the CCD threshold) crosses a thin
  // trigger slab without stopping.
  std::unique_ptr<World> fast = make_world();
  const Entity slab =
      (fast != nullptr)
          ? add_trigger_box(*fast, Vec3(0, 0, 0), Vec3(0.05F, 2.0F, 2.0F))
          : kInvalidEntity;
  const Entity bullet = (fast != nullptr)
                            ? add_ball(*fast, Vec3(-3, 0, 0), BodyType::Dynamic,
                                       Vec3(120.0F, 0.0F, 0.0F))
                            : kInvalidEntity;
  check((slab != kInvalidEntity) && (bullet != kInvalidEntity) &&
            run_frames(*fast, 4U),
        "ccd: scene runs");
  RigidBody body{};
  Transform where{};
  check(fast->get_rigid_body(bullet, &body) && (body.velocity.x == 120.0F),
        "ccd: the slab leaves the bullet's velocity unchanged");
  check(fast->get_transform(bullet, &where) && (where.position.x > 4.0F),
        "ccd: the bullet is past the slab");

  // A fast body whose only collider is a trigger crosses a thin solid wall:
  // a trigger sweeps for nothing.
  std::unique_ptr<World> ghost = make_world();
  const BodyType dynamic = BodyType::Dynamic;
  const Entity thinWall =
      (ghost != nullptr)
          ? add_collider_entity(*ghost, Vec3(0, 0, 0), ColliderShape::AABB,
                                Vec3(0.05F, 2.0F, 2.0F), false, nullptr)
          : kInvalidEntity;
  const Entity probe =
      (ghost != nullptr)
          ? add_collider_entity(*ghost, Vec3(-3, 0, 0), ColliderShape::Sphere,
                                Vec3(0.5F, 0.5F, 0.5F), true, &dynamic,
                                Vec3(120.0F, 0.0F, 0.0F))
          : kInvalidEntity;
  check((thinWall != kInvalidEntity) && (probe != kInvalidEntity) &&
            run_frames(*ghost, 4U),
        "ccd: trigger-body scene runs");
  check(ghost->get_rigid_body(probe, &body) && (body.velocity.x == 120.0F) &&
            ghost->get_transform(probe, &where) && (where.position.x > 4.0F),
        "ccd: a trigger body crosses a thin wall unswept");
}

void check_trigger_has_no_mass_properties() {
  std::unique_ptr<World> plain = make_world();
  std::unique_ptr<World> withTrigger = make_world();
  check((plain != nullptr) && (withTrigger != nullptr), "mass: worlds");
  if ((plain == nullptr) || (withTrigger == nullptr)) {
    return;
  }
  const Entity a = add_ball(*plain, Vec3(0, 0, 0), BodyType::Dynamic);
  const Entity b = add_ball(*withTrigger, Vec3(0, 0, 0), BodyType::Dynamic);
  Transform childAt{};
  childAt.position = Vec3(4.0F, 0.0F, 0.0F);
  childAt.parentId = withTrigger->persistent_id(b);
  const Entity child = withTrigger->create_scene_object(childAt);
  Collider volume{};
  volume.halfExtents = Vec3(3.0F, 3.0F, 3.0F);
  volume.isTrigger = true;
  check((a != kInvalidEntity) && (b != kInvalidEntity) &&
            withTrigger->add_collider(child, volume),
        "mass: scene builds");
  RigidBody bodyA{};
  RigidBody bodyB{};
  check(plain->get_rigid_body(a, &bodyA) &&
            withTrigger->get_rigid_body(b, &bodyB) &&
            (bodyA.inverseInertia.x == bodyB.inverseInertia.x) &&
            (bodyA.inverseInertia.y == bodyB.inverseInertia.y) &&
            (bodyA.inverseInertia.z == bodyB.inverseInertia.z),
        "mass: a child trigger leaves the body's inertia unchanged");
}

void check_overlaps_are_world_state() {
  reset_recorder();
  std::unique_ptr<World> world = make_world();
  PassThroughScene scene{};
  check((world != nullptr) && build_pass_through(*world, true, &scene),
        "state: scene builds");
  if (g_failures != 0) {
    return;
  }
  check(run_frames(*world, 15U), "state: frames before the enter run");
  engine::runtime::StateHashSections before{};
  static_cast<void>(world->state_hash(&before));
  check(run_frames(*world, 1U) && (g_eventCount == 1U),
        "state: the enter frame runs");
  engine::runtime::StateHashSections inside{};
  static_cast<void>(world->state_hash(&inside));
  check(before.physics != inside.physics,
        "state: the overlap set enters the physics hash section");

  // A copy carries the set: stepping it reports no second enter.
  std::unique_ptr<World> copy(new (std::nothrow) World());
  check(copy != nullptr, "state: copy allocated");
  if (copy == nullptr) {
    return;
  }
  *copy = *world;
  engine::runtime::set_trigger_dispatch(*copy, &record_trigger_events);
  g_eventCount = 0U;
  check(run_frames(*copy, 5U) && (g_eventCount == 0U),
        "state: a copied world keeps its overlaps");

  // A content reset forgets them without reporting an end.
  engine::physics::reset_physics_content(world->physics_context());
  check(world->physics_context().shapeStore->triggerOverlapCount == 0U,
        "state: reset clears the set");
  check(run_frames(*world, 1U) && (g_eventCount == 1U) &&
            event_is(0U, scene.trigger, scene.ball, true),
        "state: after a reset the overlap begins again");
}

} // namespace

int main() {
  check_body_passes_through_with_enter_and_exit();
  check_which_pairs_report();
  check_sleeping_body_stays_inside();
  check_destroy_and_disable_report_exit();
  check_catchup_frames_deliver_the_same_events();
  check_order_follows_identity_not_storage();
  check_overflow_keeps_the_previous_set();
  check_queries_and_ccd_ignore_triggers();
  check_trigger_has_no_mass_properties();
  check_overlaps_are_world_state();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d trigger check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
