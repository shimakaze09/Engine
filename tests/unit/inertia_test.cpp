// Verifies the rigid-body inverse inertia tensor: the analytic shape
// formulas, placement (parallel axis, rotated colliders, compound mass
// split), the world-space application, and the contract through the
// production World and physics step -- a body installed with a collider
// derives its tensor, a locked axis stays locked under joint and contact
// impulses while the free axes respond, and scenes and prefabs migrate the
// former scalar field.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/cvar.h"
#include "engine/core/json.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/inertia.h"
#include "engine/physics/physics.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

using World = engine::runtime::World;
using Entity = engine::runtime::Entity;
using Transform = engine::runtime::Transform;
using RigidBody = engine::runtime::RigidBody;
using Collider = engine::runtime::Collider;
using ColliderShape = engine::runtime::ColliderShape;
namespace math = engine::math;
namespace physics = engine::physics;

constexpr float kDt = 1.0F / 60.0F;
// One float division separates a derived axis from its closed form, so a
// relative tolerance a few ulps wide is the strictest that holds.
constexpr float kRelTolerance = 4.0e-6F;
constexpr const char *kPrefabPath = "inertia_test_prefab_temp.json";

int g_failures = 0;

void check(bool condition, const char *name) noexcept {
  if (!condition) {
    std::printf("FAIL: %s\n", name);
    ++g_failures;
  }
}

bool near_rel(float actual, float expected) noexcept {
  const float scale = std::fmax(std::fabs(expected), 1.0F);
  return std::fabs(actual - expected) <= kRelTolerance * scale;
}

bool vec_near(const math::Vec3 &actual, const math::Vec3 &expected) noexcept {
  return near_rel(actual.x, expected.x) && near_rel(actual.y, expected.y) &&
         near_rel(actual.z, expected.z);
}

bool vec_exact(const math::Vec3 &a, const math::Vec3 &b) noexcept {
  return (a.x == b.x) && (a.y == b.y) && (a.z == b.z);
}

Collider make_box(float hx, float hy, float hz) noexcept {
  Collider collider{};
  collider.shape = ColliderShape::AABB;
  collider.halfExtents = math::Vec3(hx, hy, hz);
  return collider;
}

Collider make_sphere(float radius) noexcept {
  Collider collider{};
  collider.shape = ColliderShape::Sphere;
  collider.halfExtents = math::Vec3(radius, radius, radius);
  return collider;
}

Collider make_capsule(float radius, float halfHeight) noexcept {
  Collider collider{};
  collider.shape = ColliderShape::Capsule;
  collider.halfExtents = math::Vec3(radius, halfHeight, radius);
  return collider;
}

// ---- Shape formulas --------------------------------------------------------

void test_unit_cube_and_box_axes() noexcept {
  // Unit cube of mass 1: I = (0.5^2 + 0.5^2) / 3 = 1/6 about every axis.
  check(vec_near(physics::inverse_inertia_for_collider(
                     make_box(0.5F, 0.5F, 0.5F), 1.0F),
                 math::Vec3(6.0F, 6.0F, 6.0F)),
        "unit cube inverse inertia is 6 on every axis");
  // Half extents (1, 2, 3): Ix = (4 + 9) / 3, Iy = (1 + 9) / 3,
  // Iz = (1 + 4) / 3.
  check(vec_near(physics::inverse_inertia_for_collider(
                     make_box(1.0F, 2.0F, 3.0F), 1.0F),
                 math::Vec3(3.0F / 13.0F, 3.0F / 10.0F, 3.0F / 5.0F)),
        "box inverse inertia follows (b^2 + c^2) / 3 per axis");
  // Inverse mass scales the whole tensor.
  check(vec_near(physics::inverse_inertia_for_collider(
                     make_box(1.0F, 2.0F, 3.0F), 0.25F),
                 math::Vec3(0.75F / 13.0F, 0.75F / 10.0F, 0.75F / 5.0F)),
        "box inverse inertia scales with inverse mass");
}

void test_sphere_and_capsule() noexcept {
  // Sphere radius 0.5, mass 2: I = 0.4 m r^2 = 0.2 -> inverse 5.
  check(vec_near(physics::inverse_inertia_for_collider(make_sphere(0.5F),
                                                       0.5F),
                 math::Vec3(5.0F, 5.0F, 5.0F)),
        "sphere inverse inertia is 1 / (0.4 m r^2)");

  const math::Vec3 capsule =
      physics::inverse_inertia_for_collider(make_capsule(0.5F, 1.0F), 1.0F);
  check(capsule.x == capsule.z, "capsule is symmetric about its axis");
  check(capsule.y > capsule.x,
        "capsule resists rotation less about its own axis");
  // Every part turns about the axis like a cylinder (0.5 m r^2) or a
  // sphere (0.4 m r^2), so with r^2 = 0.25 the axial inverse lies strictly
  // between 8 and 10.
  check((capsule.y > 8.0F) && (capsule.y < 10.0F),
        "capsule axial inverse inertia lies between cylinder and sphere");

  // A capsule whose cylinder vanishes is a sphere.
  const math::Vec3 degenerate =
      physics::inverse_inertia_for_collider(make_capsule(0.5F, 1.0e-4F),
                                            1.0F);
  const math::Vec3 sphere =
      physics::inverse_inertia_for_collider(make_sphere(0.5F), 1.0F);
  check(std::fabs(degenerate.x - sphere.x) <= 1.0e-3F * sphere.x &&
            std::fabs(degenerate.y - sphere.y) <= 1.0e-3F * sphere.y,
        "capsule with no cylinder matches the sphere within 0.1%");
}

void test_static_degenerate_and_clamp() noexcept {
  check(vec_exact(physics::inverse_inertia_for_collider(
                      make_box(0.5F, 0.5F, 0.5F), 0.0F),
                  math::Vec3(0.0F, 0.0F, 0.0F)),
        "static body answers a zero tensor");
  check(vec_exact(physics::inverse_inertia_for_collider(
                      make_box(0.0F, 1.0F, 1.0F), 1.0F),
                  math::Vec3(0.0F, 0.0F, 0.0F)),
        "zero-volume collider contributes nothing and locks rotation");
  // Sphere radius 1 mm, mass 1 kg: 1 / (0.4e-6) exceeds the runaway bound.
  check(vec_exact(physics::inverse_inertia_for_collider(make_sphere(1.0e-3F),
                                                        1.0F),
                  math::Vec3(physics::kMaxInverseInertia,
                             physics::kMaxInverseInertia,
                             physics::kMaxInverseInertia)),
        "tiny shapes clamp to kMaxInverseInertia per axis");
  physics::InertiaAccumulator empty{};
  check(vec_exact(physics::finish_inverse_inertia(empty, 1.0F),
                  math::Vec3(0.0F, 0.0F, 0.0F)),
        "an empty accumulator answers zero");
}

void test_parallel_axis_and_compound() noexcept {
  // Two unit cubes at +-1 on X, total mass 2 (inverse 0.5): about X each
  // cube keeps 1/6; about Y and Z each adds d^2 = 1: I = 2 * (1/6 + 1) =
  // 7/3 -> inverse 3/7. About X: 1/3 -> inverse 3.
  physics::InertiaAccumulator accumulator{};
  physics::accumulate_collider_inertia(&accumulator, make_box(0.5F, 0.5F, 0.5F),
                                       math::Vec3(1.0F, 0.0F, 0.0F),
                                       math::Quat());
  physics::accumulate_collider_inertia(&accumulator, make_box(0.5F, 0.5F, 0.5F),
                                       math::Vec3(-1.0F, 0.0F, 0.0F),
                                       math::Quat());
  check(vec_near(physics::finish_inverse_inertia(accumulator, 0.5F),
                 math::Vec3(3.0F, 3.0F / 7.0F, 3.0F / 7.0F)),
        "offset colliders add the parallel-axis term");

  // Density splits the mass: spheres of radius 0.5 at +-1 on X with
  // densities 1 and 3 hold 1/4 and 3/4 of a unit mass; about Y:
  // sum m_i (0.4 r^2 + 1) = 1.1; about X: 0.1.
  physics::InertiaAccumulator weighted{};
  Collider light = make_sphere(0.5F);
  light.density = 1.0F;
  Collider heavy = make_sphere(0.5F);
  heavy.density = 3.0F;
  physics::accumulate_collider_inertia(&weighted, light,
                                       math::Vec3(1.0F, 0.0F, 0.0F),
                                       math::Quat());
  physics::accumulate_collider_inertia(&weighted, heavy,
                                       math::Vec3(-1.0F, 0.0F, 0.0F),
                                       math::Quat());
  check(vec_near(physics::finish_inverse_inertia(weighted, 1.0F),
                 math::Vec3(10.0F, 1.0F / 1.1F, 1.0F / 1.1F)),
        "density weights the compound mass split");
}

void test_rotated_collider() noexcept {
  // A (1, 2, 3) box turned a quarter turn about Z swaps its X and Y axes.
  Collider box = make_box(1.0F, 2.0F, 3.0F);
  box.localRotation = math::from_axis_angle(math::Vec3(0.0F, 0.0F, 1.0F),
                                            0.5F * 3.14159265358979F);
  check(vec_near(physics::inverse_inertia_for_collider(box, 1.0F),
                 math::Vec3(3.0F / 10.0F, 3.0F / 13.0F, 3.0F / 5.0F)),
        "collider local rotation turns the diagonal");
}

// ---- World-space application ----------------------------------------------

void test_world_application() noexcept {
  const math::Vec3 inertia(2.0F, 3.0F, 5.0F);
  check(vec_exact(physics::apply_inverse_inertia(inertia, math::Quat(),
                                                 math::Vec3(1.0F, 1.0F, 1.0F)),
                  math::Vec3(2.0F, 3.0F, 5.0F)),
        "identity rotation scales per axis");
  // A quarter turn about Z maps world X onto body Y.
  const math::Quat quarter = math::from_axis_angle(
      math::Vec3(0.0F, 0.0F, 1.0F), 0.5F * 3.14159265358979F);
  check(vec_near(physics::apply_inverse_inertia(inertia, quarter,
                                                math::Vec3(1.0F, 0.0F, 0.0F)),
                 math::Vec3(3.0F, 0.0F, 0.0F)),
        "world X uses the body Y axis after a quarter turn about Z");
  // Lever Y, direction X: the arm is -Z, so the term is the Z axis.
  check(physics::angular_effective_inverse_mass(
            inertia, math::Quat(), math::Vec3(0.0F, 1.0F, 0.0F),
            math::Vec3(1.0F, 0.0F, 0.0F)) == 5.0F,
        "effective inverse mass picks the axis of lever x direction");
  check(physics::angular_effective_inverse_mass(
            math::Vec3(0.0F, 0.0F, 0.0F), math::Quat(),
            math::Vec3(0.0F, 1.0F, 0.0F), math::Vec3(1.0F, 0.0F, 0.0F)) == 0.0F,
        "a locked body has no angular effective inverse mass");
  float world[3][3] = {};
  physics::inverse_inertia_world(inertia, math::Quat(), world);
  check((world[0][0] == 2.0F) && (world[1][1] == 3.0F) &&
            (world[2][2] == 5.0F) && (world[0][1] == 0.0F) &&
            (world[1][2] == 0.0F) && (world[2][0] == 0.0F),
        "identity world tensor is the diagonal");
}

// ---- World derivation ------------------------------------------------------

std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
    engine::core::cvar_set_int("physics.solver_iterations", 16);
    engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);
  }
  return world;
}

Entity make_body(World &world, const math::Vec3 &position, float inverseMass,
                 const Collider *collider) noexcept {
  const Entity entity = world.create_entity();
  Transform transform{};
  transform.position = position;
  if (!world.add_transform(entity, transform)) {
    return engine::runtime::kInvalidEntity;
  }
  RigidBody body{};
  body.inverseMass = inverseMass;
  if (!world.add_rigid_body(entity, body)) {
    return engine::runtime::kInvalidEntity;
  }
  if ((collider != nullptr) && !world.add_collider(entity, *collider)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

void test_world_derives_on_collider_install() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    check(false, "world allocation");
    return;
  }
  const Collider box = make_box(1.0F, 2.0F, 3.0F);
  const Entity bodyThenCollider =
      make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), 1.0F, &box);
  RigidBody stored{};
  check(world->get_rigid_body(bodyThenCollider, &stored) &&
            vec_near(stored.inverseInertia,
                     math::Vec3(3.0F / 13.0F, 3.0F / 10.0F, 3.0F / 5.0F)),
        "add_collider derives the tensor of a default body");

  // Collider first, body second: add_rigid_body derives.
  const Entity colliderThenBody = world->create_entity();
  Transform transform{};
  transform.position = math::Vec3(10.0F, 0.0F, 0.0F);
  RigidBody body{};
  body.inverseMass = 2.0F;
  check(world->add_transform(colliderThenBody, transform) &&
            world->add_collider(colliderThenBody, box) &&
            world->add_rigid_body(colliderThenBody, body) &&
            world->get_rigid_body(colliderThenBody, &stored) &&
            vec_near(stored.inverseInertia,
                     math::Vec3(6.0F / 13.0F, 6.0F / 10.0F, 6.0F / 5.0F)),
        "add_rigid_body derives from an existing collider");

  // An authored tensor survives collider installation.
  const Entity authored = world->create_entity();
  transform.position = math::Vec3(20.0F, 0.0F, 0.0F);
  RigidBody authoredBody{};
  authoredBody.inverseMass = 1.0F;
  authoredBody.inverseInertia = math::Vec3(0.0F, 4.0F, 0.0F);
  authoredBody.inertiaAuthored = true;
  check(world->add_transform(authored, transform) &&
            world->add_rigid_body(authored, authoredBody) &&
            world->add_collider(authored, box) &&
            world->get_rigid_body(authored, &stored) &&
            vec_exact(stored.inverseInertia, math::Vec3(0.0F, 4.0F, 0.0F)),
        "an authored tensor is kept when a collider is installed");

  // No collider: the body keeps the default until one arrives.
  const Entity bare =
      make_body(*world, math::Vec3(30.0F, 0.0F, 0.0F), 1.0F, nullptr);
  check(world->get_rigid_body(bare, &stored) &&
            vec_exact(stored.inverseInertia, math::default_inverse_inertia()),
        "a body without colliders keeps the default tensor");

  // A static body never derives: it keeps the default so a later mass
  // change derives against the colliders it has then.
  const Entity fixed =
      make_body(*world, math::Vec3(40.0F, 0.0F, 0.0F), 0.0F, &box);
  check(world->get_rigid_body(fixed, &stored) &&
            vec_exact(stored.inverseInertia, math::default_inverse_inertia()),
        "a static body keeps the default tensor");

  // Replacing a derived body's collider re-derives; removing it returns
  // the body to the default.
  const Collider cube = make_box(0.5F, 0.5F, 0.5F);
  check(world->add_collider(bodyThenCollider, cube) &&
            world->get_rigid_body(bodyThenCollider, &stored) &&
            vec_near(stored.inverseInertia, math::Vec3(6.0F, 6.0F, 6.0F)),
        "replacing the collider re-derives a derived tensor");
  check(world->add_collider(authored, cube) &&
            world->get_rigid_body(authored, &stored) &&
            vec_exact(stored.inverseInertia, math::Vec3(0.0F, 4.0F, 0.0F)),
        "replacing the collider keeps an authored tensor");
  check(world->remove_collider(bodyThenCollider) &&
            world->get_rigid_body(bodyThenCollider, &stored) &&
            vec_exact(stored.inverseInertia, math::default_inverse_inertia()),
        "removing the only collider restores the default");
}

void test_world_compound_children() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    check(false, "world allocation");
    return;
  }
  // Root body with no collider of its own; two unit-cube children at +-1 on
  // X, total inverse mass 0.5: the parallel-axis case above.
  const Entity root =
      make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), 0.5F, nullptr);
  const float offsets[2] = {1.0F, -1.0F};
  for (float offset : offsets) {
    const Entity child = world->create_entity();
    Transform transform{};
    transform.position = math::Vec3(offset, 0.0F, 0.0F);
    transform.parentId = world->persistent_id(root);
    if (!world->add_transform(child, transform) ||
        !world->add_collider(child, make_box(0.5F, 0.5F, 0.5F))) {
      check(false, "child collider setup");
      return;
    }
  }
  RigidBody stored{};
  check(world->get_rigid_body(root, &stored) &&
            vec_near(stored.inverseInertia,
                     math::Vec3(3.0F, 3.0F / 7.0F, 3.0F / 7.0F)),
        "child colliders combine into the owner's tensor as they arrive");
}

/// Ownership follows the same ancestor walk collision uses: a collider any
/// depth below the body counts, placed by the transforms composed down
/// from the body; a descendant with its own body owns its subtree.
void test_world_ownership_depth_and_placement() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    check(false, "world allocation");
    return;
  }
  const Entity root =
      make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), 0.5F, nullptr);
  // An empty intermediate at +1 on X, its child at +0 -> the cube sits at
  // +1; a second empty at -1 with a child at +0 -> the cube sits at -1.
  // Same geometry as the compound-children case, one level deeper.
  const float offsets[2] = {1.0F, -1.0F};
  Entity cubes[2] = {};
  for (std::size_t i = 0U; i < 2U; ++i) {
    const Entity middle = world->create_entity();
    Transform transform{};
    transform.position = math::Vec3(offsets[i], 0.0F, 0.0F);
    transform.parentId = world->persistent_id(root);
    const Entity cube = world->create_entity();
    Transform cubeTransform{};
    cubeTransform.parentId = world->persistent_id(middle);
    if (!world->add_transform(middle, transform) ||
        !world->add_transform(cube, cubeTransform) ||
        !world->add_collider(cube, make_box(0.5F, 0.5F, 0.5F))) {
      check(false, "grandchild collider setup");
      return;
    }
    cubes[i] = cube;
  }
  RigidBody stored{};
  check(world->get_rigid_body(root, &stored) &&
            vec_near(stored.inverseInertia,
                     math::Vec3(3.0F, 3.0F / 7.0F, 3.0F / 7.0F)),
        "grandchild colliders count through the ancestor walk");

  // Moving a grandchild re-derives: both cubes at the origin make one
  // double-weight unit cube, I = 2/6 -> inverse 3 about every axis.
  Transform moved{};
  moved.parentId = world->persistent_id(root);
  for (const Entity cube : cubes) {
    Transform local{};
    if (!world->get_transform(cube, &local)) {
      check(false, "grandchild transform read");
      return;
    }
    local.position = math::Vec3(0.0F, 0.0F, 0.0F);
    if (!world->add_transform(cube, local)) {
      check(false, "grandchild transform write");
      return;
    }
  }
  // The intermediates still sit at +-1, so re-home each cube at the root.
  for (const Entity cube : cubes) {
    if (!world->add_transform(cube, moved)) {
      check(false, "grandchild reparent to root");
      return;
    }
  }
  check(world->get_rigid_body(root, &stored) &&
            vec_near(stored.inverseInertia, math::Vec3(3.0F, 3.0F, 3.0F)),
        "moving and reparenting owned colliders re-derives the owner");

  // Reparenting a cube under a second body moves its geometry between
  // owners: the root (mass 2) keeps one unit cube, I = 2/6 -> inverse 3;
  // the other body (mass 1) gains one, I = 1/6 -> inverse 6.
  const Entity other =
      make_body(*world, math::Vec3(10.0F, 0.0F, 0.0F), 1.0F, nullptr);
  Transform underOther{};
  underOther.parentId = world->persistent_id(other);
  check(world->add_transform(cubes[1], underOther) &&
            world->get_rigid_body(root, &stored) &&
            vec_near(stored.inverseInertia, math::Vec3(3.0F, 3.0F, 3.0F)),
        "the previous owner loses a reparented collider");
  check(world->get_rigid_body(other, &stored) &&
            vec_near(stored.inverseInertia, math::Vec3(6.0F, 6.0F, 6.0F)),
        "the new owner gains a reparented collider");

  // Destroying the remaining owned cube returns the root to the default.
  check(world->destroy_entity(cubes[0]) &&
            world->get_rigid_body(root, &stored) &&
            vec_exact(stored.inverseInertia, math::default_inverse_inertia()),
        "destroying the last owned collider re-derives the owner");
}

/// Provenance is explicit: an authored tensor equal to the default is kept
/// beside a collider, and handing a body back to automatic re-derives.
void test_world_provenance_is_explicit() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    check(false, "world allocation");
    return;
  }
  const Entity entity = world->create_entity();
  Transform transform{};
  RigidBody body{};
  body.inverseMass = 1.0F;
  body.inverseInertia = math::default_inverse_inertia();
  body.inertiaAuthored = true;
  RigidBody stored{};
  check(world->add_transform(entity, transform) &&
            world->add_rigid_body(entity, body) &&
            world->add_collider(entity, make_box(0.5F, 0.5F, 0.5F)) &&
            world->get_rigid_body(entity, &stored) &&
            vec_exact(stored.inverseInertia, math::default_inverse_inertia()) &&
            stored.inertiaAuthored,
        "an authored default-looking tensor is never derived over");
  body.inertiaAuthored = false;
  check(world->add_rigid_body(entity, body) &&
            world->get_rigid_body(entity, &stored) &&
            vec_near(stored.inverseInertia, math::Vec3(6.0F, 6.0F, 6.0F)) &&
            !stored.inertiaAuthored,
        "switching a body to automatic derives from its collider");
  body.inverseMass = 2.0F;
  check(world->add_rigid_body(entity, body) &&
            world->get_rigid_body(entity, &stored) &&
            vec_near(stored.inverseInertia, math::Vec3(12.0F, 12.0F, 12.0F)),
        "a mass change re-derives an automatic tensor");
}

// ---- Behaviour through the fixed step --------------------------------------

bool step_world(World &world, Entity body) noexcept {
  RigidBody *rigidBody = world.get_rigid_body_ptr(body);
  if (rigidBody != nullptr) {
    rigidBody->sleeping = false;
    rigidBody->sleepFrameCount = 0U;
  }
  world.begin_update_phase();
  const bool ok = engine::runtime::step_physics(world, kDt) &&
                  engine::runtime::resolve_collisions(world, kDt);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
  return ok;
}

// A ball-socket joint pins a spinning body to a static anchor. Removing the
// anchor's relative velocity needs a torque about Z; with Z locked the
// solver can only translate the body, so its spin about Z survives exactly
// as it would without the joint (air damping only) and the free axes stay
// untouched. With Z free the same impulse slows the spin.
float joint_spin_after_steps(const math::Vec3 &inverseInertia,
                             bool withJoint) noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return -1.0F;
  }
  const Entity anchor =
      make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), 0.0F, nullptr);
  const Entity spinner =
      make_body(*world, math::Vec3(1.0F, 0.0F, 0.0F), 1.0F, nullptr);
  RigidBody *body = world->get_rigid_body_ptr(spinner);
  if (body == nullptr) {
    return -1.0F;
  }
  body->inverseInertia = inverseInertia;
  body->angularVelocity = math::Vec3(0.0F, 0.0F, 1.0F);
  if (withJoint &&
      (engine::runtime::add_ball_socket_joint(*world, anchor, spinner,
                                              math::Vec3(0.0F, 0.0F, 0.0F)) ==
       physics::kInvalidJointId)) {
    return -1.0F;
  }
  for (int i = 0; i < 5; ++i) {
    if (!step_world(*world, spinner)) {
      return -1.0F;
    }
  }
  RigidBody after{};
  if (!world->get_rigid_body(spinner, &after)) {
    return -1.0F;
  }
  check((after.angularVelocity.x == 0.0F) && (after.angularVelocity.y == 0.0F),
        "joint impulse leaves the untouched axes at exactly zero");
  return after.angularVelocity.z;
}

void test_joint_respects_locked_axis() noexcept {
  const float control =
      joint_spin_after_steps(math::Vec3(5.0F, 5.0F, 5.0F), false);
  check((control > 0.0F) && (control <= 1.0F), "unjointed spinner control");
  const float lockedSpin =
      joint_spin_after_steps(math::Vec3(5.0F, 5.0F, 0.0F), true);
  check(lockedSpin == control,
        "ball-socket joint cannot spin down a body locked about Z");
  const float freeSpin =
      joint_spin_after_steps(math::Vec3(5.0F, 5.0F, 5.0F), true);
  check((freeSpin >= 0.0F) && (freeSpin < control),
        "ball-socket joint spins down a body free about Z");
}

// A cube tilted about Z rests on one edge above a static floor, so the
// contact torque acts about Z. Sequential impulses at the two edge points
// pass transient torque about the other axes, so the assertion is on the
// locked axes: in the body frame they read zero on every step, whichever
// ones they are, while a free Z axis turns the cube. Every angular impulse
// lands orthogonal to a locked body axis and integrating the rotation about
// the angular velocity keeps that orthogonality, so the only residue is the
// rounding of the two quaternion rotations that map into the body frame.
struct TiltRun final {
  bool ok = false;
  bool lockedAxesStayedZero = true;
  float tiltChange = 0.0F;
};

TiltRun run_tilted_cube(const math::Vec3 &inverseInertia) noexcept {
  TiltRun run{};
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return run;
  }
  engine::runtime::set_gravity(*world, 0.0F, -9.81F, 0.0F);
  const Collider floor = make_box(10.0F, 0.5F, 10.0F);
  const Entity ground =
      make_body(*world, math::Vec3(0.0F, -0.5F, 0.0F), 0.0F, &floor);
  if (ground == engine::runtime::kInvalidEntity) {
    return run;
  }
  const Entity cube = world->create_entity();
  Transform transform{};
  transform.position = math::Vec3(0.0F, 0.75F, 0.0F);
  transform.rotation = math::from_axis_angle(math::Vec3(0.0F, 0.0F, 1.0F),
                                             0.35F);
  RigidBody body{};
  body.inverseMass = 1.0F;
  body.inverseInertia = inverseInertia;
  body.inertiaAuthored = true;
  if (!world->add_transform(cube, transform) ||
      !world->add_rigid_body(cube, body) ||
      !world->add_collider(cube, make_box(0.5F, 0.5F, 0.5F))) {
    return run;
  }
  for (int i = 0; i < 60; ++i) {
    if (!step_world(*world, cube)) {
      return run;
    }
    RigidBody after{};
    if (!world->get_rigid_body(cube, &after)) {
      return run;
    }
    Transform current{};
    if (!world->get_transform(cube, &current)) {
      return run;
    }
    const math::Vec3 bodySpin = math::rotate_vector(
        after.angularVelocity,
        math::conjugate(math::normalize(current.rotation)));
    const float residue =
        1.0e-5F * std::fmax(1.0F, math::length(after.angularVelocity));
    if (((inverseInertia.x == 0.0F) && (std::fabs(bodySpin.x) > residue)) ||
        ((inverseInertia.y == 0.0F) && (std::fabs(bodySpin.y) > residue)) ||
        ((inverseInertia.z == 0.0F) && (std::fabs(bodySpin.z) > residue))) {
      run.lockedAxesStayedZero = false;
    }
  }
  Transform afterTransform{};
  if (!world->get_transform(cube, &afterTransform)) {
    return run;
  }
  const float tilt = 2.0F * std::atan2(std::fabs(afterTransform.rotation.z),
                                       std::fabs(afterTransform.rotation.w));
  run.tiltChange = std::fabs(tilt - 0.35F);
  run.ok = true;
  return run;
}

void test_contact_respects_locked_axis() noexcept {
  const TiltRun lockedZ = run_tilted_cube(math::Vec3(6.0F, 6.0F, 0.0F));
  check(lockedZ.ok && lockedZ.lockedAxesStayedZero,
        "edge contact never spins a cube locked about Z");
  const TiltRun onlyZ = run_tilted_cube(math::Vec3(0.0F, 0.0F, 6.0F));
  check(onlyZ.ok && onlyZ.lockedAxesStayedZero,
        "edge contact never spins a cube about its locked X and Y");
  check(onlyZ.ok && (onlyZ.tiltChange > 0.05F),
        "edge contact turns a cube free about Z");
  const TiltRun fullyLocked = run_tilted_cube(math::Vec3(0.0F, 0.0F, 0.0F));
  check(fullyLocked.ok && fullyLocked.lockedAxesStayedZero &&
            (fullyLocked.tiltChange == 0.0F),
        "a fully locked cube keeps its tilt exactly");
}

// ---- Serialization migration ----------------------------------------------

constexpr const char *kSceneV2Scalar =
    "{\"version\":2,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0.0,0.0,0.0]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":0.25}}}]}";
constexpr const char *kSceneV2DefaultWithCollider =
    "{\"version\":2,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0.0,0.0,0.0]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":1.0},"
    "\"Collider\":{\"shape\":0,\"halfExtents\":[0.5,0.5,0.5]}}}]}";
constexpr const char *kSceneV1Scalar =
    "{\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0.0,0.0,0.0]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":0.0}}}]}";
constexpr const char *kSceneV3Scalar =
    "{\"version\":3,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0.0,0.0,0.0]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":0.25}}}]}";
constexpr const char *kSceneV3Array =
    "{\"version\":3,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0.0,0.0,0.0]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":[0.25,0.5,0.125]}}"
    "}]}";
constexpr const char *kSceneV3DefaultArrayWithCollider =
    "{\"version\":3,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0,0,0]},"
    "\"Collider\":{\"halfExtents\":[0.5,0.5,0.5]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":[1,1,1]}}}]}";
constexpr const char *kSceneV4AutomaticWithCollider =
    "{\"version\":4,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0,0,0]},"
    "\"Collider\":{\"halfExtents\":[0.5,0.5,0.5]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":[1,1,1],"
    "\"inertiaAuthored\":false}}}]}";
constexpr const char *kSceneV4AuthoredWithCollider =
    "{\"version\":4,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0,0,0]},"
    "\"Collider\":{\"halfExtents\":[0.5,0.5,0.5]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":[1,1,1],"
    "\"inertiaAuthored\":true}}}]}";
constexpr const char *kSceneV4NoProvenanceWithCollider =
    "{\"version\":4,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0,0,0]},"
    "\"Collider\":{\"halfExtents\":[0.5,0.5,0.5]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":[1,1,1]}}}]}";
constexpr const char *kSceneV3ShortArray =
    "{\"version\":3,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0.0,0.0,0.0]},"
    "\"RigidBody\":{\"inverseMass\":1.0,\"inverseInertia\":[0.25,0.5]}}}]}";

bool load_scene_body(const char *json, RigidBody *outBody) noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return false;
  }
  if (!engine::runtime::load_scene(*world, json, std::strlen(json))) {
    return false;
  }
  const Entity entity = world->find_entity_by_persistent_id(7U);
  return (entity != engine::runtime::kInvalidEntity) &&
         world->get_rigid_body(entity, outBody);
}

void test_scene_migration() noexcept {
  RigidBody body{};
  check(load_scene_body(kSceneV2Scalar, &body) &&
            vec_exact(body.inverseInertia, math::Vec3(0.25F, 0.25F, 0.25F)),
        "scene v2 scalar inverse inertia loads on every axis");
  check(load_scene_body(kSceneV1Scalar, &body) &&
            vec_exact(body.inverseInertia, math::Vec3(0.0F, 0.0F, 0.0F)),
        "scene v1 scalar zero stays a locked body");
  // Older revisions always wrote the number the scene simulated with, so
  // it stays that number, authored: a default-looking 1 is never read as
  // "derive from the collider".
  check(load_scene_body(kSceneV2DefaultWithCollider, &body) &&
            vec_exact(body.inverseInertia, math::Vec3(1.0F, 1.0F, 1.0F)) &&
            body.inertiaAuthored,
        "scene v2 default scalar is kept as the authored value");
  check(load_scene_body(kSceneV3Array, &body) &&
            vec_exact(body.inverseInertia, math::Vec3(0.25F, 0.5F, 0.125F)) &&
            body.inertiaAuthored,
        "scene v3 array loads per axis, authored");
  check(load_scene_body(kSceneV3DefaultArrayWithCollider, &body) &&
            vec_exact(body.inverseInertia, math::Vec3(1.0F, 1.0F, 1.0F)) &&
            body.inertiaAuthored,
        "scene v3 default array with a collider is kept as authored");
  // The current revision carries provenance: an automatic body derives from
  // its collider on load whatever number the document holds, an authored
  // one keeps its number.
  check(load_scene_body(kSceneV4AutomaticWithCollider, &body) &&
            vec_near(body.inverseInertia, math::Vec3(6.0F, 6.0F, 6.0F)) &&
            !body.inertiaAuthored,
        "scene v4 automatic body derives from the collider on load");
  check(load_scene_body(kSceneV4AuthoredWithCollider, &body) &&
            vec_exact(body.inverseInertia, math::Vec3(1.0F, 1.0F, 1.0F)) &&
            body.inertiaAuthored,
        "scene v4 authored body keeps its number beside a collider");
  check(load_scene_body(kSceneV4NoProvenanceWithCollider, &body) &&
            vec_near(body.inverseInertia, math::Vec3(6.0F, 6.0F, 6.0F)) &&
            !body.inertiaAuthored,
        "scene v4 without the provenance key is automatic");

  // The current revision reads the field strictly; a refused load leaves
  // the destination untouched.
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    check(false, "world allocation");
    return;
  }
  const Entity sentinel =
      make_body(*world, math::Vec3(0.0F, 0.0F, 0.0F), 1.0F, nullptr);
  const std::size_t aliveBefore = world->alive_entity_count();
  check(!engine::runtime::load_scene(*world, kSceneV3Scalar,
                                     std::strlen(kSceneV3Scalar)),
        "scene v3 refuses a scalar inverse inertia");
  check(!engine::runtime::load_scene(*world, kSceneV3ShortArray,
                                     std::strlen(kSceneV3ShortArray)),
        "scene v3 refuses a two-element inverse inertia");
  RigidBody kept{};
  check((world->alive_entity_count() == aliveBefore) &&
            world->get_rigid_body(sentinel, &kept),
        "refused scene loads leave the world unchanged");
}

void test_scene_round_trip() noexcept {
  std::unique_ptr<World> source = make_world();
  if (source == nullptr) {
    check(false, "world allocation");
    return;
  }
  const Entity entity = source->create_scene_object();
  Transform transform{};
  RigidBody body{};
  body.inverseMass = 1.0F;
  body.inverseInertia = math::Vec3(0.25F, 0.5F, 0.125F);
  body.inertiaAuthored = true;
  if (!source->add_transform(entity, transform) ||
      !source->add_rigid_body(entity, body)) {
    check(false, "round-trip source setup");
    return;
  }
  const engine::runtime::PersistentId id = source->persistent_id(entity);
  // A second, automatic body with a collider: its provenance must survive
  // the trip so the loaded world derives it rather than trusting the number.
  const Entity automatic = source->create_scene_object();
  RigidBody automaticBody{};
  automaticBody.inverseMass = 1.0F;
  Transform automaticTransform{};
  automaticTransform.position = math::Vec3(5.0F, 0.0F, 0.0F);
  if (!source->add_transform(automatic, automaticTransform) ||
      !source->add_rigid_body(automatic, automaticBody) ||
      !source->add_collider(automatic, make_box(0.5F, 0.5F, 0.5F))) {
    check(false, "round-trip automatic source setup");
    return;
  }
  const engine::runtime::PersistentId automaticId =
      source->persistent_id(automatic);

  std::unique_ptr<char[]> buffer(
      new (std::nothrow) char[engine::core::JsonWriter::kBufferBytes]);
  if (buffer == nullptr) {
    check(false, "buffer allocation");
    return;
  }
  std::size_t size = 0U;
  check(engine::runtime::save_scene(*source, buffer.get(),
                                    engine::core::JsonWriter::kBufferBytes,
                                    &size),
        "save_scene to buffer");
  engine::core::JsonParser parser{};
  engine::core::JsonValue versionValue{};
  std::uint32_t version = 0U;
  check(parser.parse(buffer.get(), size) && (parser.root() != nullptr) &&
            parser.get_object_field(*parser.root(), "version",
                                    &versionValue) &&
            parser.as_uint(versionValue, &version) && (version == 4U),
        "saved scene carries revision 4");
  check(std::strstr(buffer.get(), "\"inverseInertia\":[") != nullptr,
        "saved scene writes the tensor as an array");

  std::unique_ptr<World> loaded = make_world();
  if (loaded == nullptr) {
    check(false, "world allocation");
    return;
  }
  RigidBody restored{};
  const bool ok = engine::runtime::load_scene(*loaded, buffer.get(), size);
  const Entity loadedEntity = loaded->find_entity_by_persistent_id(id);
  check(ok && (loadedEntity != engine::runtime::kInvalidEntity) &&
            loaded->get_rigid_body(loadedEntity, &restored) &&
            vec_exact(restored.inverseInertia, body.inverseInertia) &&
            restored.inertiaAuthored,
        "scene round trip restores the authored tensor exactly");
  const Entity loadedAutomatic =
      loaded->find_entity_by_persistent_id(automaticId);
  check(ok && (loadedAutomatic != engine::runtime::kInvalidEntity) &&
            loaded->get_rigid_body(loadedAutomatic, &restored) &&
            !restored.inertiaAuthored &&
            vec_near(restored.inverseInertia, math::Vec3(6.0F, 6.0F, 6.0F)),
        "scene round trip keeps an automatic body automatic and derived");

  // Byte-identical re-save.
  std::unique_ptr<char[]> second(
      new (std::nothrow) char[engine::core::JsonWriter::kBufferBytes]);
  std::size_t secondSize = 0U;
  check((second != nullptr) &&
            engine::runtime::save_scene(*loaded, second.get(),
                                        engine::core::JsonWriter::kBufferBytes,
                                        &secondSize) &&
            (secondSize == size) &&
            (std::memcmp(buffer.get(), second.get(), size) == 0),
        "re-saving the loaded scene is byte-identical");
}

bool write_prefab_file(const char *text) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kPrefabPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kPrefabPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t size = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

void test_prefab_migration() noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    check(false, "world allocation");
    return;
  }
  RigidBody body{};

  check(write_prefab_file("{\"version\":1,\"components\":{"
                          "\"RigidBody\":{\"inverseMass\":1.0,"
                          "\"inverseInertia\":0.25}}}"),
        "write prefab v1");
  Entity entity = engine::runtime::instantiate_prefab(*world, kPrefabPath);
  check((entity != engine::runtime::kInvalidEntity) &&
            world->get_rigid_body(entity, &body) &&
            vec_exact(body.inverseInertia, math::Vec3(0.25F, 0.25F, 0.25F)),
        "prefab v1 scalar inverse inertia loads on every axis");

  check(write_prefab_file("{\"version\":2,\"components\":{"
                          "\"RigidBody\":{\"inverseMass\":1.0,"
                          "\"inverseInertia\":[0.25,0.5,0.125]}}}"),
        "write prefab v2");
  entity = engine::runtime::instantiate_prefab(*world, kPrefabPath);
  check((entity != engine::runtime::kInvalidEntity) &&
            world->get_rigid_body(entity, &body) &&
            vec_exact(body.inverseInertia, math::Vec3(0.25F, 0.5F, 0.125F)),
        "prefab v2 array loads per axis");

  const std::size_t aliveBefore = world->alive_entity_count();
  check(write_prefab_file("{\"version\":2,\"components\":{"
                          "\"RigidBody\":{\"inverseMass\":1.0,"
                          "\"inverseInertia\":0.25}}}"),
        "write prefab v2 scalar");
  entity = engine::runtime::instantiate_prefab(*world, kPrefabPath);
  check((entity == engine::runtime::kInvalidEntity) &&
            (world->alive_entity_count() == aliveBefore),
        "prefab v2 refuses a scalar inverse inertia and creates nothing");
  static_cast<void>(std::remove(kPrefabPath));
}

} // namespace

int main() {
  test_unit_cube_and_box_axes();
  test_sphere_and_capsule();
  test_static_degenerate_and_clamp();
  test_parallel_axis_and_compound();
  test_rotated_collider();
  test_world_application();
  test_world_derives_on_collider_install();
  test_world_compound_children();
  test_world_ownership_depth_and_placement();
  test_world_provenance_is_explicit();
  test_joint_respects_locked_axis();
  test_contact_respects_locked_axis();
  test_scene_migration();
  test_scene_round_trip();
  test_prefab_migration();
  if (g_failures != 0) {
    std::printf("inertia tests: %d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
