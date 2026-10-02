// Verifies the kinematic character move and the segment-to-convex closest
// points it is built on, through the production physics over a real World:
// - segment_convex_distance is exact against a box (axis-aligned and
//   rotated) and within GJK's tolerance against a sphere and a capsule,
//   and reports an intersecting segment;
// - a grounded character walks a floor keeping its skin gap, stops at a
//   wall, slides along it, climbs a step no higher than its step offset
//   and not one higher, walks up a walkable ramp but not a steep one,
//   stops under a ceiling, and never passes through a thin wall at speed;
// - it is pushed out of a box it starts inside, stays on the ground
//   walking down a ramp, and ignores triggers, colliders on layers the
//   matrix keeps apart, and its own collider;
// - the same move twice gives the same bits, and a non-finite move is
//   refused;
// - through the runtime, a character moves its entity and records grounded
//   and its flags; a controller setting out of range is refused; and each
//   refused move (outside the Input phase, no controller, no unrotated
//   capsule, parented, scaled unevenly, a dynamic body, a non-finite
//   displacement) leaves the entity where it was.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "engine/math/quat.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/physics/character_move.h"
#include "engine/physics/collider.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/runtime/character_controller.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

using engine::math::Vec3;
using engine::physics::CharacterCapsule;
using engine::physics::CharacterMoveResult;
using engine::physics::CharacterMoveSettings;
using engine::runtime::Entity;
using engine::runtime::World;

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

bool near(float a, float b, float tolerance) noexcept {
  return std::fabs(a - b) <= tolerance;
}

constexpr float kRadius = 0.3F;
constexpr float kHeight = 1.8F;
constexpr float kSkin = 0.02F;
constexpr float kPi = 3.14159265F;

std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
  }
  return world;
}

/// A static collider: no body, so it never moves.
Entity add_static(World &world, engine::runtime::ColliderShape shape,
                  const Vec3 &position, const Vec3 &halfExtents,
                  const engine::math::Quat &rotation = engine::math::Quat(),
                  std::uint32_t layer = 1U, bool isTrigger = false) noexcept {
  const Entity entity = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = position;
  transform.rotation = rotation;
  engine::runtime::Collider collider{};
  collider.shape = shape;
  collider.halfExtents = halfExtents;
  collider.collisionLayer = layer;
  collider.isTrigger = isTrigger;
  static_cast<void>(world.add_transform(entity, transform));
  static_cast<void>(world.add_collider(entity, collider));
  return entity;
}

Entity add_box(World &world, const Vec3 &position, const Vec3 &halfExtents,
               const engine::math::Quat &rotation = engine::math::Quat()) {
  return add_static(world, engine::runtime::ColliderShape::AABB, position,
                    halfExtents, rotation);
}

/// A floor whose top is y = 0.
void add_floor(World &world) {
  static_cast<void>(
      add_box(world, Vec3(0.0F, -0.5F, 0.0F), Vec3(50.0F, 0.5F, 50.0F)));
}

/// The capsule of a character whose feet are at `feet`.
CharacterCapsule capsule_at(const Vec3 &feet) noexcept {
  return CharacterCapsule{
      engine::math::add(feet, Vec3(0.0F, kRadius, 0.0F)),
      engine::math::add(feet, Vec3(0.0F, kHeight - kRadius, 0.0F)), kRadius};
}

CharacterMoveSettings settings(bool wasGrounded) noexcept {
  CharacterMoveSettings s{};
  s.slopeLimitCos = std::cos(45.0F * kPi / 180.0F);
  s.stepOffset = 0.3F;
  s.skinWidth = kSkin;
  s.wasGrounded = wasGrounded;
  return s;
}

/// Feet resting on the floor at the skin gap.
const Vec3 kStanding(0.0F, kSkin, 0.0F);

CharacterMoveResult move(const World &world, const Vec3 &feet,
                         const Vec3 &displacement,
                         const CharacterMoveSettings &s) noexcept {
  CharacterMoveResult result{};
  if (!engine::physics::move_character(world, capsule_at(feet), displacement, s,
                                       &result)) {
    std::fprintf(stderr, "  move refused\n");
  }
  return result;
}

void check_segment_distance() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "distance: world");
  if (world == nullptr) {
    return;
  }
  const auto geometry = [&](Entity entity) {
    engine::physics::ColliderWorldGeometry shape{};
    engine::runtime::Collider collider{};
    engine::runtime::Transform transform{};
    static_cast<void>(world->get_collider(entity, &collider));
    static_cast<void>(world->get_transform(entity, &transform));
    static_cast<void>(engine::physics::make_collider_world_geometry(
        collider,
        engine::math::compose_trs(transform.position, transform.rotation,
                                  transform.scale),
        nullptr, &shape));
    return shape;
  };
  engine::physics::SegmentConvexDistance d{};

  const Entity box =
      add_box(*world, Vec3(0.0F, 0.0F, 0.0F), Vec3(1.0F, 1.0F, 1.0F));
  check(engine::physics::segment_convex_distance(Vec3(3.0F, -0.5F, 0.0F),
                                                 Vec3(3.0F, 0.5F, 0.0F),
                                                 geometry(box), &d) &&
            !d.intersecting && near(d.distance, 2.0F, 1.0e-6F) &&
            near(d.onShape.x, 1.0F, 1.0e-6F) &&
            near(d.onSegment.x, 3.0F, 1.0e-6F),
        "distance: a segment beside a box face is exact");
  check(
      engine::physics::segment_convex_distance(
          Vec3(2.0F, 2.0F, 0.0F), Vec3(4.0F, 4.0F, 0.0F), geometry(box), &d) &&
          near(d.distance, std::sqrt(2.0F), 1.0e-5F) &&
          near(d.onShape.x, 1.0F, 1.0e-5F) && near(d.onShape.y, 1.0F, 1.0e-5F),
      "distance: a segment off a box corner meets the corner");
  check(engine::physics::segment_convex_distance(Vec3(-3.0F, 0.0F, 0.0F),
                                                 Vec3(3.0F, 0.0F, 0.0F),
                                                 geometry(box), &d) &&
            d.intersecting && (d.distance == 0.0F),
        "distance: a segment through a box intersects it");

  const engine::math::Quat yaw45 =
      engine::math::from_euler(0.0F, 45.0F * kPi / 180.0F, 0.0F);
  const Entity turned =
      add_box(*world, Vec3(0.0F, 10.0F, 0.0F), Vec3(1.0F, 1.0F, 1.0F), yaw45);
  check(engine::physics::segment_convex_distance(Vec3(3.0F, 9.5F, 0.0F),
                                                 Vec3(3.0F, 10.5F, 0.0F),
                                                 geometry(turned), &d) &&
            near(d.distance, 3.0F - std::sqrt(2.0F), 1.0e-5F),
        "distance: a turned box's edge is found exactly");
  // The closest points always agree with the distance, whichever way GJK
  // ended: a witness pair from a different simplex than the distance
  // gives a normal that is not unit length.
  bool consistent = true;
  for (int i = 0; i < 64; ++i) {
    const float x = 1.5F + (0.05F * static_cast<float>(i));
    const float y = 9.0F + (0.03F * static_cast<float>(i));
    if (engine::physics::segment_convex_distance(
            Vec3(x, y, 0.3F), Vec3(x, y + 1.2F, 0.3F), geometry(turned), &d) &&
        !d.intersecting) {
      const float pointGap =
          engine::math::length(engine::math::sub(d.onSegment, d.onShape));
      consistent = consistent && near(pointGap, d.distance, 1.0e-4F);
    }
  }
  check(consistent, "distance: the closest points always span the distance");

  const Entity sphere =
      add_static(*world, engine::runtime::ColliderShape::Sphere,
                 Vec3(0.0F, 20.0F, 0.0F), Vec3(1.0F, 1.0F, 1.0F));
  check(engine::physics::segment_convex_distance(Vec3(-1.0F, 23.0F, 0.0F),
                                                 Vec3(1.0F, 23.0F, 0.0F),
                                                 geometry(sphere), &d) &&
            near(d.distance, 2.0F, 2.0e-5F) &&
            near(d.onShape.y, 21.0F, 1.0e-3F),
        "distance: a sphere is found within GJK's tolerance");

  const Entity capsule =
      add_static(*world, engine::runtime::ColliderShape::Capsule,
                 Vec3(0.0F, 30.0F, 0.0F), Vec3(0.5F, 1.0F, 0.5F));
  check(engine::physics::segment_convex_distance(Vec3(2.0F, 29.0F, 0.0F),
                                                 Vec3(2.0F, 31.0F, 0.0F),
                                                 geometry(capsule), &d) &&
            near(d.distance, 1.5F, 2.0e-5F),
        "distance: a capsule's side is found within GJK's tolerance");
}

void check_floor_and_walls() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "walls: world");
  if (world == nullptr) {
    return;
  }
  add_floor(*world);
  // A wall whose near face is x = 1.5.
  static_cast<void>(
      add_box(*world, Vec3(2.0F, 2.0F, 0.0F), Vec3(0.5F, 2.0F, 20.0F)));

  CharacterMoveResult r =
      move(*world, kStanding, Vec3(1.0F, -0.1F, 0.0F), settings(true));
  check(near(r.translation.x, 1.0F, 1.0e-4F) &&
            near(r.translation.y, 0.0F, 1.0e-3F) && r.grounded &&
            ((r.flags & engine::physics::kCharacterCollidedBelow) != 0U),
        "walls: a grounded walk keeps the skin gap above the floor");

  const float stopX = 1.5F - kRadius - kSkin;
  r = move(*world, kStanding, Vec3(3.0F, 0.0F, 0.0F), settings(true));
  check(near(r.translation.x, stopX, 2.0e-3F) &&
            ((r.flags & engine::physics::kCharacterCollidedSides) != 0U),
        "walls: a wall stops the character its skin short");

  r = move(*world, kStanding, Vec3(3.0F, 0.0F, 3.0F), settings(true));
  check(near(r.translation.x, stopX, 2.0e-3F) &&
            near(r.translation.z, 3.0F, 1.0e-3F),
        "walls: a diagonal walk slides along the wall, keeping its full "
        "run along it");

  // A thin wall at x = 5, met at 10 m in one move.
  static_cast<void>(
      add_box(*world, Vec3(5.0F, 2.0F, 10.0F), Vec3(0.025F, 2.0F, 2.0F)));
  r = move(*world, Vec3(0.0F, kSkin, 10.0F), Vec3(10.0F, 0.0F, 0.0F),
           settings(true));
  check(r.translation.x < 5.0F - 0.025F - kRadius,
        "walls: a long move never passes through a thin wall");

  // A ceiling whose underside is y = 2.5.
  static_cast<void>(
      add_box(*world, Vec3(-10.0F, 3.0F, 0.0F), Vec3(1.0F, 0.5F, 1.0F)));
  r = move(*world, Vec3(-10.0F, kSkin, 0.0F), Vec3(0.0F, 2.0F, 0.0F),
           settings(true));
  // The feet start a skin above the floor and the head stops a skin
  // under the ceiling.
  const float headroom = 2.5F - kHeight - (2.0F * kSkin);
  check(near(r.translation.y, headroom, 2.0e-3F) &&
            ((r.flags & engine::physics::kCharacterCollidedAbove) != 0U) &&
            !r.grounded,
        "walls: a ceiling stops a jump its skin short");
}

void check_steps() {
  for (const float height : {0.2F, 0.45F}) {
    std::unique_ptr<World> world = make_world();
    check(world != nullptr, "steps: world");
    if (world == nullptr) {
      return;
    }
    add_floor(*world);
    // A ledge from x = 1 to x = 5.
    static_cast<void>(add_box(*world, Vec3(3.0F, height * 0.5F, 0.0F),
                              Vec3(2.0F, height * 0.5F, 5.0F)));
    const CharacterMoveResult r =
        move(*world, kStanding, Vec3(2.0F, -0.05F, 0.0F), settings(true));
    if (height < 0.3F) {
      check(near(r.translation.y, height, 2.0e-3F) &&
                (r.translation.x > 1.9F) && r.grounded,
            "steps: a ledge under the step offset is climbed");
    } else {
      check((r.translation.y < 0.01F) &&
                near(r.translation.x, 1.0F - kRadius - kSkin, 2.0e-3F),
            "steps: a ledge over the step offset blocks");
    }
  }
}

void check_ramps() {
  for (const float degrees : {30.0F, 60.0F}) {
    std::unique_ptr<World> world = make_world();
    check(world != nullptr, "ramps: world");
    if (world == nullptr) {
      return;
    }
    add_floor(*world);
    // A long ramp rising toward +x, its surface through (2, 0, 0).
    const float angle = degrees * kPi / 180.0F;
    const engine::math::Quat tilt = engine::math::from_euler(0.0F, 0.0F, angle);
    const float halfLength = 10.0F;
    const Vec3 surfaceCenterOffset(-std::sin(angle) * 0.5F,
                                   std::cos(angle) * 0.5F, 0.0F);
    const Vec3 rampCenter(
        2.0F + (halfLength * std::cos(angle)) - surfaceCenterOffset.x,
        (halfLength * std::sin(angle)) - surfaceCenterOffset.y, 0.0F);
    static_cast<void>(
        add_box(*world, rampCenter, Vec3(halfLength, 0.5F, 5.0F), tilt));
    // Walk onto the ramp a tenth of a metre at a time.
    Vec3 feet(1.5F, kSkin, 0.0F);
    bool grounded = true;
    for (int stepIndex = 0; stepIndex < 30; ++stepIndex) {
      const CharacterMoveResult r =
          move(*world, feet, Vec3(0.1F, -0.02F, 0.0F), settings(grounded));
      feet = engine::math::add(feet, r.translation);
      grounded = r.grounded;
    }
    if (degrees < 45.0F) {
      check((feet.y > 0.3F) && grounded, "ramps: a walkable ramp is walked up");
    } else {
      check(feet.y < 0.1F, "ramps: a steep ramp is a wall");
    }
  }

  // Walking down a 20 degree ramp keeps the character on it.
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "down: world");
  if (world == nullptr) {
    return;
  }
  const float angle = 20.0F * kPi / 180.0F;
  static_cast<void>(add_box(*world, Vec3(0.0F, -0.5F, 0.0F),
                            Vec3(20.0F, 0.5F, 5.0F),
                            engine::math::from_euler(0.0F, 0.0F, -angle)));
  Vec3 feet(-3.0F, 3.0F * std::tan(angle) + 0.05F, 0.0F);
  CharacterMoveResult r =
      move(*world, feet, Vec3(0.0F, -0.5F, 0.0F), settings(false));
  feet = engine::math::add(feet, r.translation);
  bool alwaysGrounded = r.grounded;
  for (int stepIndex = 0; stepIndex < 40; ++stepIndex) {
    r = move(*world, feet, Vec3(0.15F, 0.0F, 0.0F), settings(true));
    feet = engine::math::add(feet, r.translation);
    alwaysGrounded = alwaysGrounded && r.grounded;
  }
  check(alwaysGrounded && (feet.x > 2.5F),
        "down: walking down a ramp stays on the ground every step");
}

void check_depenetration_and_filters() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "filters: world");
  if (world == nullptr) {
    return;
  }
  add_floor(*world);
  // A box the character starts 0.1 inside.
  static_cast<void>(add_box(*world, Vec3(kRadius + 0.4F, 1.0F, 0.0F),
                            Vec3(0.5F, 1.0F, 1.0F)));
  CharacterMoveResult r =
      move(*world, kStanding, Vec3(0.0F, 0.0F, 0.0F), settings(true));
  check(r.translation.x < -0.099F && r.translation.x > -0.12F,
        "filters: a character inside a box is pushed out of it");

  // A trigger, a box on a layer the matrix keeps apart, and the
  // character's own collider, each across the path at x = 3.
  const Entity trigger = add_static(
      *world, engine::runtime::ColliderShape::AABB, Vec3(3.0F, 1.0F, 10.0F),
      Vec3(0.2F, 1.0F, 1.0F), engine::math::Quat(), 1U, true);
  static_cast<void>(trigger);
  static_cast<void>(add_static(*world, engine::runtime::ColliderShape::AABB,
                               Vec3(3.0F, 1.0F, 20.0F), Vec3(0.2F, 1.0F, 1.0F),
                               engine::math::Quat(), 1U << 5U));
  const Entity own =
      add_static(*world, engine::runtime::ColliderShape::Capsule,
                 Vec3(3.0F, 0.9F, 30.0F), Vec3(kRadius, 0.6F, kRadius));
  engine::physics::CollisionLayerMatrix matrix{};
  matrix.rows[0] &= ~(1U << 5U);
  matrix.rows[5] &= ~1U;
  engine::physics::set_collision_matrix(*world, matrix);

  CharacterMoveSettings s = settings(true);
  r = move(*world, Vec3(0.0F, kSkin, 10.0F), Vec3(6.0F, 0.0F, 0.0F), s);
  check(near(r.translation.x, 6.0F, 1.0e-4F),
        "filters: a trigger never blocks");
  r = move(*world, Vec3(0.0F, kSkin, 20.0F), Vec3(6.0F, 0.0F, 0.0F), s);
  check(near(r.translation.x, 6.0F, 1.0e-4F),
        "filters: a layer the matrix keeps apart never blocks");
  r = move(*world, Vec3(0.0F, kSkin, 30.0F), Vec3(6.0F, 0.0F, 0.0F), s);
  check(r.translation.x < 3.0F, "filters: another capsule blocks");
  s.self = own;
  r = move(*world, Vec3(0.0F, kSkin, 30.0F), Vec3(6.0F, 0.0F, 0.0F), s);
  check(near(r.translation.x, 6.0F, 1.0e-4F),
        "filters: the character's own collider never blocks");
}

void check_determinism_and_refusals() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "determinism: world");
  if (world == nullptr) {
    return;
  }
  add_floor(*world);
  static_cast<void>(
      add_box(*world, Vec3(2.0F, 2.0F, 0.0F), Vec3(0.5F, 2.0F, 2.0F)));
  const CharacterMoveResult a =
      move(*world, kStanding, Vec3(2.5F, -0.3F, 1.7F), settings(true));
  const CharacterMoveResult b =
      move(*world, kStanding, Vec3(2.5F, -0.3F, 1.7F), settings(true));
  check(std::memcmp(&a.translation, &b.translation, sizeof(Vec3)) == 0 &&
            (a.flags == b.flags) && (a.grounded == b.grounded),
        "determinism: the same move gives the same bits");

  CharacterMoveResult r{};
  r.flags = 7U;
  check(!engine::physics::move_character(
            *world, capsule_at(kStanding),
            Vec3(std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F),
            settings(true), &r) &&
            (r.flags == 0U),
        "refusals: a non-finite move is refused and clears the result");
}

/// A character entity standing on the floor with its feet at `feet`.
Entity add_character(World &world, const Vec3 &feet) noexcept {
  const Entity entity = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = feet;
  engine::runtime::Collider collider{};
  collider.shape = engine::runtime::ColliderShape::Capsule;
  collider.halfExtents = Vec3(kRadius, (kHeight * 0.5F) - kRadius, kRadius);
  collider.localPosition = Vec3(0.0F, kHeight * 0.5F, 0.0F);
  engine::runtime::CharacterControllerComponent controller{};
  static_cast<void>(world.add_transform(entity, transform));
  static_cast<void>(world.add_collider(entity, collider));
  static_cast<void>(world.add_character_controller(entity, controller));
  return entity;
}

Vec3 position_of(const World &world, Entity entity) noexcept {
  engine::runtime::Transform transform{};
  static_cast<void>(world.get_transform(entity, &transform));
  return transform.position;
}

void check_runtime_move() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "runtime: world");
  if (world == nullptr) {
    return;
  }
  add_floor(*world);

  engine::runtime::CharacterControllerComponent bad{};
  const Entity probe = world->create_entity();
  bad.slopeLimit = 90.0F;
  const bool steep = world->add_character_controller(probe, bad);
  bad = engine::runtime::CharacterControllerComponent{};
  bad.skinWidth = 0.0F;
  const bool skinless = world->add_character_controller(probe, bad);
  bad = engine::runtime::CharacterControllerComponent{};
  bad.stepOffset = std::numeric_limits<float>::quiet_NaN();
  const bool nan = world->add_character_controller(probe, bad);
  check(!steep && !skinless && !nan && !world->has_character_controller(probe),
        "runtime: a setting out of its range is refused");

  const Entity hero = add_character(*world, kStanding);
  engine::runtime::CharacterMoveOutcome outcome{};
  check(engine::runtime::move_character(*world, hero, Vec3(1.0F, -0.1F, 0.0F),
                                        &outcome) &&
            outcome.grounded &&
            near(position_of(*world, hero).x, 1.0F, 1.0e-4F),
        "runtime: a character walks its entity along the floor");
  engine::runtime::CharacterControllerComponent stored{};
  check(world->get_character_controller(hero, &stored) && stored.grounded &&
            ((stored.collisionFlags &
              engine::physics::kCharacterCollidedBelow) != 0U),
        "runtime: the controller records grounded and what it touched");

  // Each refusal leaves the entity where it was.
  const auto refused = [&](Entity entity, const Vec3 &displacement) {
    const Vec3 before = position_of(*world, entity);
    const bool moved =
        engine::runtime::move_character(*world, entity, displacement, nullptr);
    const Vec3 after = position_of(*world, entity);
    return !moved && (std::memcmp(&before, &after, sizeof(Vec3)) == 0);
  };
  check(refused(hero, Vec3(std::numeric_limits<float>::infinity(), 0.0F, 0.0F)),
        "runtime: a non-finite displacement is refused");
  world->begin_update_phase();
  check(refused(hero, Vec3(1.0F, 0.0F, 0.0F)),
        "runtime: a move outside the Input phase is refused");
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  const Entity noController = add_character(*world, Vec3(0.0F, kSkin, 5.0F));
  static_cast<void>(world->remove_character_controller(noController));
  check(refused(noController, Vec3(1.0F, 0.0F, 0.0F)),
        "runtime: an entity without a controller is refused");

  const Entity boxy = add_character(*world, Vec3(0.0F, kSkin, 10.0F));
  engine::runtime::Collider box{};
  static_cast<void>(world->add_collider(boxy, box));
  check(refused(boxy, Vec3(1.0F, 0.0F, 0.0F)),
        "runtime: a character without a Capsule Collider is refused");

  const Entity tilted = add_character(*world, Vec3(0.0F, kSkin, 15.0F));
  engine::runtime::Collider tiltedCollider{};
  static_cast<void>(world->get_collider(tilted, &tiltedCollider));
  tiltedCollider.localRotation = engine::math::from_euler(0.5F, 0.0F, 0.0F);
  static_cast<void>(world->add_collider(tilted, tiltedCollider));
  check(refused(tilted, Vec3(1.0F, 0.0F, 0.0F)),
        "runtime: a rotated capsule is refused");

  const Entity stretched = add_character(*world, Vec3(0.0F, kSkin, 20.0F));
  engine::runtime::Transform stretch{};
  static_cast<void>(world->get_transform(stretched, &stretch));
  stretch.scale = Vec3(1.0F, 2.0F, 1.0F);
  static_cast<void>(world->add_transform(stretched, stretch));
  check(refused(stretched, Vec3(1.0F, 0.0F, 0.0F)),
        "runtime: an unevenly scaled character is refused");

  const Entity parent = world->create_scene_object();
  const Entity child = add_character(*world, Vec3(0.0F, kSkin, 25.0F));
  engine::runtime::Transform childTransform{};
  static_cast<void>(world->get_transform(child, &childTransform));
  childTransform.parentId = world->persistent_id(parent);
  check(world->add_transform(child, childTransform) &&
            refused(child, Vec3(1.0F, 0.0F, 0.0F)),
        "runtime: a parented character is refused");

  const Entity dynamic = add_character(*world, Vec3(0.0F, kSkin, 30.0F));
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  static_cast<void>(world->add_rigid_body(dynamic, body));
  check(refused(dynamic, Vec3(1.0F, 0.0F, 0.0F)),
        "runtime: a character with a dynamic body is refused");
  body.bodyType =
      static_cast<std::uint32_t>(engine::runtime::BodyType::Kinematic);
  static_cast<void>(world->add_rigid_body(dynamic, body));
  check(engine::runtime::move_character(*world, dynamic, Vec3(1.0F, 0.0F, 0.0F),
                                        nullptr) &&
            near(position_of(*world, dynamic).x, 1.0F, 1.0e-4F),
        "runtime: a character with a kinematic body moves");
}

} // namespace

int main() {
  check_segment_distance();
  check_floor_and_walls();
  check_steps();
  check_ramps();
  check_depenetration_and_filters();
  check_determinism_and_refusals();
  check_runtime_move();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d character move check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
