// Regression tests for issue #111: single-point contacts must produce
// angular response against static geometry, and friction must transfer
// linear motion into rotation through the contact lever arm.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
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

/// Off-center single-point capsule impact on a static box edge must spin
/// the capsule: the contact lever arm is not parallel to the normal, so a
/// torque is required even though the other endpoint is static.
static void test_offcenter_static_impact_produces_rotation() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto box = world->create_entity();
  engine::runtime::Transform boxT{};
  world->add_transform(box, boxT);
  engine::runtime::Collider boxCol{};
  boxCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(box, boxCol);
  engine::runtime::RigidBody boxRB{};
  boxRB.inverseMass = 0.0F;
  world->add_rigid_body(box, boxRB);

  const auto capsule = world->create_entity();
  engine::runtime::Transform capT{};
  capT.position = engine::math::Vec3(0.6F, 1.1F, 0.0F);
  world->add_transform(capsule, capT);
  engine::runtime::Collider capCol{};
  capCol.shape = engine::runtime::ColliderShape::Capsule;
  capCol.halfExtents = engine::math::Vec3(0.25F, 0.5F, 0.25F);
  world->add_collider(capsule, capCol);
  engine::runtime::RigidBody capRB{};
  capRB.inverseMass = 1.0F;
  capRB.velocity = engine::math::Vec3(0.0F, -3.0F, 0.0F);
  world->add_rigid_body(capsule, capRB);

  run_step(*world);

  const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(capsule);
  if (body == nullptr) {
    check(false, "Capsule body lookup failed");
    return;
  }
  const float angSpeed = engine::math::length(body->angularVelocity);
  check(angSpeed > 1.0e-3F,
        "Off-center static impact produces angular velocity");
  check(body->velocity.y > -3.0F,
        "Off-center static impact removes approach velocity");
}

/// A sphere sliding on a high-friction static floor must start rolling:
/// friction at the contact point converts linear motion into spin about
/// -Z for +X travel (point velocity vx + 0.5*wz approaches zero).
static void test_sphere_rolls_on_static_floor() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();

  const auto floor = world->create_entity();
  engine::runtime::Transform floorT{};
  world->add_transform(floor, floorT);
  engine::runtime::Collider floorCol{};
  floorCol.halfExtents = engine::math::Vec3(50.0F, 0.5F, 50.0F);
  floorCol.staticFriction = 0.9F;
  floorCol.dynamicFriction = 0.8F;
  world->add_collider(floor, floorCol);
  engine::runtime::RigidBody floorRB{};
  floorRB.inverseMass = 0.0F;
  world->add_rigid_body(floor, floorRB);

  const auto sphere = world->create_entity();
  engine::runtime::Transform sphereT{};
  sphereT.position = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  world->add_transform(sphere, sphereT);
  engine::runtime::Collider sphereCol{};
  sphereCol.shape = engine::runtime::ColliderShape::Sphere;
  sphereCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  sphereCol.staticFriction = 0.9F;
  sphereCol.dynamicFriction = 0.8F;
  world->add_collider(sphere, sphereCol);
  engine::runtime::RigidBody sphereRB{};
  sphereRB.inverseMass = 1.0F;
  sphereRB.velocity = engine::math::Vec3(3.0F, 0.0F, 0.0F);
  world->add_rigid_body(sphere, sphereRB);

  for (int i = 0; i < 30; ++i) {
    run_step(*world);
  }

  const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(sphere);
  if (body == nullptr) {
    check(false, "Sphere body lookup failed");
    return;
  }
  check(body->angularVelocity.z < -0.5F,
        "Friction on static floor spins the sphere toward rolling");
  check(body->velocity.x < 3.0F, "Friction reduces sliding velocity");
  check(std::fabs(body->angularVelocity.y) < 1.0e-4F,
        "No spurious yaw spin from straight sliding");
}

/// A sphere dropped dead-center onto a flat static floor has its lever arm
/// parallel to the normal and no tangential motion: angular velocity must
/// stay exactly zero.
static void test_center_drop_stays_spinless() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto floor = world->create_entity();
  engine::runtime::Transform floorT{};
  world->add_transform(floor, floorT);
  engine::runtime::Collider floorCol{};
  floorCol.halfExtents = engine::math::Vec3(50.0F, 0.5F, 50.0F);
  world->add_collider(floor, floorCol);
  engine::runtime::RigidBody floorRB{};
  floorRB.inverseMass = 0.0F;
  world->add_rigid_body(floor, floorRB);

  const auto sphere = world->create_entity();
  engine::runtime::Transform sphereT{};
  sphereT.position = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  world->add_transform(sphere, sphereT);
  engine::runtime::Collider sphereCol{};
  sphereCol.shape = engine::runtime::ColliderShape::Sphere;
  sphereCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(sphere, sphereCol);
  engine::runtime::RigidBody sphereRB{};
  sphereRB.inverseMass = 1.0F;
  sphereRB.velocity = engine::math::Vec3(0.0F, -2.0F, 0.0F);
  world->add_rigid_body(sphere, sphereRB);

  for (int i = 0; i < 5; ++i) {
    run_step(*world);
  }

  const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(sphere);
  if (body == nullptr) {
    check(false, "Sphere body lookup failed");
    return;
  }
  check((body->angularVelocity.x == 0.0F) &&
            (body->angularVelocity.y == 0.0F) &&
            (body->angularVelocity.z == 0.0F),
        "Dead-center drop introduces no spin");
}

/// A glancing dynamic-vs-dynamic sphere impact must spin both bodies via
/// friction while conserving linear momentum.
static void test_dynamic_pair_friction_spin_conserves_momentum() noexcept {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    check(false, "World allocation failed");
    return;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto sphereA = world->create_entity();
  engine::runtime::Transform aT{};
  world->add_transform(sphereA, aT);
  engine::runtime::Collider aCol{};
  aCol.shape = engine::runtime::ColliderShape::Sphere;
  aCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  aCol.staticFriction = 0.9F;
  aCol.dynamicFriction = 0.8F;
  world->add_collider(sphereA, aCol);
  engine::runtime::RigidBody aRB{};
  aRB.inverseMass = 1.0F;
  aRB.velocity = engine::math::Vec3(2.0F, 0.0F, 0.0F);
  world->add_rigid_body(sphereA, aRB);

  const auto sphereB = world->create_entity();
  engine::runtime::Transform bT{};
  bT.position = engine::math::Vec3(0.9F, 0.3F, 0.0F);
  world->add_transform(sphereB, bT);
  engine::runtime::Collider bCol{};
  bCol.shape = engine::runtime::ColliderShape::Sphere;
  bCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  bCol.staticFriction = 0.9F;
  bCol.dynamicFriction = 0.8F;
  world->add_collider(sphereB, bCol);
  engine::runtime::RigidBody bRB{};
  bRB.inverseMass = 1.0F;
  world->add_rigid_body(sphereB, bRB);

  run_step(*world);

  const engine::runtime::RigidBody *bodyA = world->get_rigid_body_ptr(sphereA);
  const engine::runtime::RigidBody *bodyB = world->get_rigid_body_ptr(sphereB);
  if ((bodyA == nullptr) || (bodyB == nullptr)) {
    check(false, "Sphere body lookup failed");
    return;
  }
  check(engine::math::length(bodyA->angularVelocity) > 1.0e-4F,
        "Glancing impact spins sphere A");
  check(engine::math::length(bodyB->angularVelocity) > 1.0e-4F,
        "Glancing impact spins sphere B");
  const engine::math::Vec3 momentum =
      engine::math::add(bodyA->velocity, bodyB->velocity);
  check((std::fabs(momentum.x - 2.0F) < 1.0e-4F) &&
            (std::fabs(momentum.y) < 1.0e-4F) &&
            (std::fabs(momentum.z) < 1.0e-4F),
        "Glancing impact conserves linear momentum");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== Contact Angular Response Tests (issue #111) ===\n");

  test_offcenter_static_impact_produces_rotation();
  test_sphere_rolls_on_static_floor();
  test_center_drop_stays_spinless();
  test_dynamic_pair_friction_spin_conserves_momentum();

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
