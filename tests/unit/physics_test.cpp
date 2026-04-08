#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

std::size_t g_dispatchedPairCount = 0U;

void test_collision_dispatch(const std::uint32_t *pairs,
                             std::size_t pairCount) noexcept {
  static_cast<void>(pairs);
  g_dispatchedPairCount = pairCount;
}

int check_gravity_step() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  world->end_frame_phase();

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 2;
  }

  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  if (!world->add_transform(entity, transform)) {
    return 3;
  }

  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  if (!world->add_rigid_body(entity, body)) {
    return 4;
  }

  world->begin_update_phase();
  if (!engine::runtime::step_physics(*world, 1.0F / 60.0F)) {
    world->end_frame_phase();
    return 5;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform updated{};
  if (!world->get_transform(entity, &updated)) {
    return 6;
  }

  if (updated.position.y >= transform.position.y) {
    return 7;
  }

  return 0;
}

int check_overlap_resolution() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 10;
  }

  world->end_frame_phase();

  const engine::runtime::Entity a = world->create_entity();
  const engine::runtime::Entity b = world->create_entity();
  if ((a == engine::runtime::kInvalidEntity) ||
      (b == engine::runtime::kInvalidEntity)) {
    return 11;
  }

  engine::runtime::Transform transformA{};
  engine::runtime::Transform transformB{};
  transformA.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  transformB.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);

  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;

  if (!world->add_transform(a, transformA) ||
      !world->add_transform(b, transformB)) {
    return 12;
  }

  if (!world->add_collider(a, collider) || !world->add_collider(b, collider)) {
    return 13;
  }

  if (!world->add_rigid_body(a, body) || !world->add_rigid_body(b, body)) {
    return 14;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 15;
  }

  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 16;
  }

  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform outA{};
  engine::runtime::Transform outB{};
  if (!world->get_transform(a, &outA) || !world->get_transform(b, &outB)) {
    return 17;
  }

  const float dx = std::fabs(outA.position.x - outB.position.x);
  const float dy = std::fabs(outA.position.y - outB.position.y);
  const float dz = std::fabs(outA.position.z - outB.position.z);
  const float sumX = collider.halfExtents.x + collider.halfExtents.x;
  const float sumY = collider.halfExtents.y + collider.halfExtents.y;
  const float sumZ = collider.halfExtents.z + collider.halfExtents.z;

  const bool stillOverlapping = (dx < sumX) && (dy < sumY) && (dz < sumZ);
  return stillOverlapping ? 18 : 0;
}

int check_static_body_immovable() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 20;
  }

  world->end_frame_phase();

  const engine::runtime::Entity staticEntity = world->create_entity();
  const engine::runtime::Entity dynamicEntity = world->create_entity();
  if ((staticEntity == engine::runtime::kInvalidEntity) ||
      (dynamicEntity == engine::runtime::kInvalidEntity)) {
    return 21;
  }

  engine::runtime::Transform staticTransform{};
  engine::runtime::Transform dynamicTransform{};
  staticTransform.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  dynamicTransform.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);

  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::RigidBody staticBody{};
  staticBody.inverseMass = 0.0F;
  engine::runtime::RigidBody dynamicBody{};
  dynamicBody.inverseMass = 1.0F;

  if (!world->add_transform(staticEntity, staticTransform) ||
      !world->add_transform(dynamicEntity, dynamicTransform)) {
    return 22;
  }

  if (!world->add_collider(staticEntity, collider) ||
      !world->add_collider(dynamicEntity, collider)) {
    return 23;
  }

  if (!world->add_rigid_body(staticEntity, staticBody) ||
      !world->add_rigid_body(dynamicEntity, dynamicBody)) {
    return 24;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 25;
  }

  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 26;
  }

  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform outStatic{};
  engine::runtime::Transform outDynamic{};
  if (!world->get_transform(staticEntity, &outStatic) ||
      !world->get_transform(dynamicEntity, &outDynamic)) {
    return 27;
  }

  const bool staticMoved =
      (outStatic.position.x != staticTransform.position.x) ||
      (outStatic.position.y != staticTransform.position.y) ||
      (outStatic.position.z != staticTransform.position.z);
  const bool dynamicMoved =
      (outDynamic.position.x != dynamicTransform.position.x) ||
      (outDynamic.position.y != dynamicTransform.position.y) ||
      (outDynamic.position.z != dynamicTransform.position.z);

  if (staticMoved) {
    return 28;
  }

  return dynamicMoved ? 0 : 29;
}

int check_angular_velocity_integration() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 30;
  }

  world->end_frame_phase();

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 31;
  }

  engine::runtime::Transform transform{};
  if (!world->add_transform(entity, transform)) {
    return 32;
  }

  engine::runtime::RigidBody body{};
  body.inverseMass = 0.0F; // static so no gravity drift
  body.inverseInertia = 1.0F;
  body.angularVelocity = engine::math::Vec3(0.0F, 3.14159F, 0.0F);
  if (!world->add_rigid_body(entity, body)) {
    return 33;
  }

  world->begin_update_phase();
  if (!engine::runtime::step_physics(*world, 1.0F / 60.0F)) {
    world->end_frame_phase();
    return 34;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform updated{};
  if (!world->get_transform(entity, &updated)) {
    return 35;
  }

  // The rotation quat should have changed from identity.
  const engine::math::Quat identity{};
  const float dotVal = engine::math::dot(updated.rotation, identity);
  if (std::fabs(dotVal) > 0.9999F) {
    return 36; // rotation did not change
  }

  return 0;
}

int check_angular_impulse_from_collision() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 40;
  }

  world->end_frame_phase();

  const engine::runtime::Entity a = world->create_entity();
  const engine::runtime::Entity b = world->create_entity();
  if ((a == engine::runtime::kInvalidEntity) ||
      (b == engine::runtime::kInvalidEntity)) {
    return 41;
  }

  engine::runtime::Transform tA{};
  tA.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(0.9F, 0.5F, 0.0F); // offset collision

  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::RigidBody bodyA{};
  bodyA.inverseMass = 1.0F;
  bodyA.inverseInertia = 1.0F;
  engine::runtime::RigidBody bodyB{};
  bodyB.inverseMass = 1.0F;
  bodyB.inverseInertia = 1.0F;
  bodyB.velocity = engine::math::Vec3(-2.0F, 0.0F, 0.0F); // approaching A

  if (!world->add_transform(a, tA) || !world->add_transform(b, tB)) {
    return 42;
  }
  if (!world->add_collider(a, collider) || !world->add_collider(b, collider)) {
    return 43;
  }
  if (!world->add_rigid_body(a, bodyA) || !world->add_rigid_body(b, bodyB)) {
    return 44;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 45;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 46;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  // At least one body should have non-zero angular velocity after off-center
  // hit.
  engine::runtime::RigidBody outA{};
  engine::runtime::RigidBody outB{};
  if (!world->get_rigid_body(a, &outA) || !world->get_rigid_body(b, &outB)) {
    return 47;
  }

  const float angSpeedA = engine::math::length(outA.angularVelocity);
  const float angSpeedB = engine::math::length(outB.angularVelocity);
  if ((angSpeedA < 1e-6F) && (angSpeedB < 1e-6F)) {
    return 48; // no angular impulse produced
  }

  return 0;
}

int check_zero_inverse_inertia_prevents_rotation() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 50;
  }

  world->end_frame_phase();

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 51;
  }

  engine::runtime::Transform transform{};
  if (!world->add_transform(entity, transform)) {
    return 52;
  }

  engine::runtime::RigidBody body{};
  body.inverseMass = 0.0F;
  body.inverseInertia = 0.0F; // rotation locked
  body.angularVelocity = engine::math::Vec3(0.0F, 5.0F, 0.0F);
  if (!world->add_rigid_body(entity, body)) {
    return 53;
  }

  world->begin_update_phase();
  if (!engine::runtime::step_physics(*world, 1.0F / 60.0F)) {
    world->end_frame_phase();
    return 54;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform updated{};
  if (!world->get_transform(entity, &updated)) {
    return 55;
  }

  // Rotation should remain identity because inverseInertia is 0.
  const engine::math::Quat identity{};
  const float dotVal = engine::math::dot(updated.rotation, identity);
  if (std::fabs(dotVal) < 0.9999F) {
    return 56; // rotation changed despite inverseInertia=0
  }

  return 0;
}

int check_high_restitution_bounce() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 60;
  }

  world->end_frame_phase();

  const engine::runtime::Entity floor = world->create_entity();
  const engine::runtime::Entity ball = world->create_entity();
  if ((floor == engine::runtime::kInvalidEntity) ||
      (ball == engine::runtime::kInvalidEntity)) {
    return 61;
  }

  engine::runtime::Transform floorT{};
  floorT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform ballT{};
  ballT.position = engine::math::Vec3(0.0F, 0.9F, 0.0F);

  engine::runtime::Collider floorCol{};
  floorCol.halfExtents = engine::math::Vec3(5.0F, 0.5F, 5.0F);
  floorCol.restitution = 1.0F;
  engine::runtime::Collider ballCol{};
  ballCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  ballCol.restitution = 1.0F;

  engine::runtime::RigidBody floorBody{};
  floorBody.inverseMass = 0.0F; // static
  engine::runtime::RigidBody ballBody{};
  ballBody.inverseMass = 1.0F;
  ballBody.velocity = engine::math::Vec3(0.0F, -5.0F, 0.0F);

  if (!world->add_transform(floor, floorT) ||
      !world->add_transform(ball, ballT)) {
    return 62;
  }
  if (!world->add_collider(floor, floorCol) ||
      !world->add_collider(ball, ballCol)) {
    return 63;
  }
  if (!world->add_rigid_body(floor, floorBody) ||
      !world->add_rigid_body(ball, ballBody)) {
    return 64;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 65;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 66;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::RigidBody outBall{};
  if (!world->get_rigid_body(ball, &outBall)) {
    return 67;
  }

  // With restitution=1.0 (perfectly elastic), the ball should bounce back
  // upward with similar speed. Velocity y should be positive.
  if (outBall.velocity.y <= 0.0F) {
    return 68; // did not bounce
  }

  return 0;
}

int check_zero_restitution_no_bounce() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 70;
  }

  world->end_frame_phase();

  const engine::runtime::Entity floor = world->create_entity();
  const engine::runtime::Entity ball = world->create_entity();
  if ((floor == engine::runtime::kInvalidEntity) ||
      (ball == engine::runtime::kInvalidEntity)) {
    return 71;
  }

  engine::runtime::Transform floorT{};
  floorT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform ballT{};
  ballT.position = engine::math::Vec3(0.0F, 0.9F, 0.0F);

  engine::runtime::Collider floorCol{};
  floorCol.halfExtents = engine::math::Vec3(5.0F, 0.5F, 5.0F);
  floorCol.restitution = 0.0F;
  engine::runtime::Collider ballCol{};
  ballCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  ballCol.restitution = 0.0F;

  engine::runtime::RigidBody floorBody{};
  floorBody.inverseMass = 0.0F;
  engine::runtime::RigidBody ballBody{};
  ballBody.inverseMass = 1.0F;
  ballBody.velocity = engine::math::Vec3(0.0F, -5.0F, 0.0F);

  if (!world->add_transform(floor, floorT) ||
      !world->add_transform(ball, ballT)) {
    return 72;
  }
  if (!world->add_collider(floor, floorCol) ||
      !world->add_collider(ball, ballCol)) {
    return 73;
  }
  if (!world->add_rigid_body(floor, floorBody) ||
      !world->add_rigid_body(ball, ballBody)) {
    return 74;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 75;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 76;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::RigidBody outBall{};
  if (!world->get_rigid_body(ball, &outBall)) {
    return 77;
  }

  // With restitution=0 (perfectly inelastic), the ball's vertical velocity
  // should be near zero (no bounce).
  if (std::fabs(outBall.velocity.y) > 0.1F) {
    return 78; // still has significant bounce
  }

  return 0;
}

int check_friction_slows_sliding() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 80;
  }

  world->end_frame_phase();

  const engine::runtime::Entity floor = world->create_entity();
  const engine::runtime::Entity slider = world->create_entity();
  if ((floor == engine::runtime::kInvalidEntity) ||
      (slider == engine::runtime::kInvalidEntity)) {
    return 81;
  }

  engine::runtime::Transform floorT{};
  floorT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform sliderT{};
  sliderT.position = engine::math::Vec3(0.0F, 0.5F, 0.0F); // overlapping top

  engine::runtime::Collider floorCol{};
  floorCol.halfExtents = engine::math::Vec3(5.0F, 0.5F, 5.0F);
  floorCol.restitution = 0.0F;
  floorCol.staticFriction = 0.8F;
  floorCol.dynamicFriction = 0.6F;
  engine::runtime::Collider sliderCol{};
  sliderCol.halfExtents = engine::math::Vec3(0.5F, 0.1F, 0.5F);
  sliderCol.restitution = 0.0F;
  sliderCol.staticFriction = 0.8F;
  sliderCol.dynamicFriction = 0.6F;

  engine::runtime::RigidBody floorBody{};
  floorBody.inverseMass = 0.0F;
  engine::runtime::RigidBody sliderBody{};
  sliderBody.inverseMass = 1.0F;
  sliderBody.velocity =
      engine::math::Vec3(10.0F, -1.0F, 0.0F); // sliding + slight downward

  if (!world->add_transform(floor, floorT) ||
      !world->add_transform(slider, sliderT)) {
    return 82;
  }
  if (!world->add_collider(floor, floorCol) ||
      !world->add_collider(slider, sliderCol)) {
    return 83;
  }
  if (!world->add_rigid_body(floor, floorBody) ||
      !world->add_rigid_body(slider, sliderBody)) {
    return 84;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 85;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 86;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::RigidBody outSlider{};
  if (!world->get_rigid_body(slider, &outSlider)) {
    return 87;
  }

  // Friction should have reduced horizontal speed from the initial 10.
  if (std::fabs(outSlider.velocity.x) >= 10.0F) {
    return 88; // friction had no effect
  }

  return 0;
}

int check_raycast_hits_aabb() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 90;
  }

  world->end_frame_phase();

  const engine::runtime::Entity box = world->create_entity();
  if (box == engine::runtime::kInvalidEntity) {
    return 91;
  }

  engine::runtime::Transform t{};
  t.position = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  if (!world->add_transform(box, t)) {
    return 92;
  }

  engine::runtime::Collider col{};
  col.halfExtents = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  col.shape = engine::runtime::ColliderShape::AABB;
  if (!world->add_collider(box, col)) {
    return 93;
  }

  // Advance to readable state.
  world->begin_update_phase();
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::PhysicsRaycastHit hit{};
  const bool found = engine::runtime::raycast(
      *world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
      engine::math::Vec3(1.0F, 0.0F, 0.0F), 100.0F, &hit);
  if (!found) {
    return 94;
  }
  // Distance should be ~4.0 (box center at 5, halfExtent 1 → near face at 4).
  if (std::fabs(hit.distance - 4.0F) > 0.1F) {
    return 95;
  }
  // Normal should be (-1, 0, 0) — the face facing the ray origin.
  if (hit.normal.x > -0.9F) {
    return 96;
  }

  return 0;
}

int check_raycast_hits_sphere() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 100;
  }

  world->end_frame_phase();

  const engine::runtime::Entity sph = world->create_entity();
  if (sph == engine::runtime::kInvalidEntity) {
    return 101;
  }

  engine::runtime::Transform t{};
  t.position = engine::math::Vec3(0.0F, 0.0F, 10.0F);
  if (!world->add_transform(sph, t)) {
    return 102;
  }

  engine::runtime::Collider col{};
  col.halfExtents = engine::math::Vec3(2.0F, 2.0F, 2.0F);
  col.shape = engine::runtime::ColliderShape::Sphere;
  if (!world->add_collider(sph, col)) {
    return 103;
  }

  world->begin_update_phase();
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::PhysicsRaycastHit hit{};
  const bool found = engine::runtime::raycast(
      *world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
      engine::math::Vec3(0.0F, 0.0F, 1.0F), 100.0F, &hit);
  if (!found) {
    return 104;
  }
  // Distance should be ~8.0 (center at z=10, radius=2 → near surface at z=8).
  if (std::fabs(hit.distance - 8.0F) > 0.1F) {
    return 105;
  }
  // Normal should point toward ray origin: (0, 0, -1).
  if (hit.normal.z > -0.9F) {
    return 106;
  }

  return 0;
}

int check_raycast_misses() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 110;
  }

  world->end_frame_phase();

  const engine::runtime::Entity box = world->create_entity();
  if (box == engine::runtime::kInvalidEntity) {
    return 111;
  }

  engine::runtime::Transform t{};
  t.position = engine::math::Vec3(5.0F, 5.0F, 0.0F);
  if (!world->add_transform(box, t)) {
    return 112;
  }

  engine::runtime::Collider col{};
  col.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_collider(box, col)) {
    return 113;
  }

  world->begin_update_phase();
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  // Ray along X axis should miss the box at (5, 5, 0).
  engine::runtime::PhysicsRaycastHit hit{};
  const bool found = engine::runtime::raycast(
      *world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
      engine::math::Vec3(1.0F, 0.0F, 0.0F), 100.0F, &hit);
  if (found) {
    return 114; // should not have hit
  }

  return 0;
}

int check_raycast_returns_closest() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 120;
  }

  world->end_frame_phase();

  const engine::runtime::Entity near = world->create_entity();
  const engine::runtime::Entity far = world->create_entity();
  if ((near == engine::runtime::kInvalidEntity) ||
      (far == engine::runtime::kInvalidEntity)) {
    return 121;
  }

  engine::runtime::Transform tNear{};
  tNear.position = engine::math::Vec3(3.0F, 0.0F, 0.0F);
  engine::runtime::Transform tFar{};
  tFar.position = engine::math::Vec3(8.0F, 0.0F, 0.0F);

  engine::runtime::Collider col{};
  col.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  if (!world->add_transform(near, tNear) || !world->add_transform(far, tFar)) {
    return 122;
  }
  if (!world->add_collider(near, col) || !world->add_collider(far, col)) {
    return 123;
  }

  world->begin_update_phase();
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::PhysicsRaycastHit hit{};
  const bool found = engine::runtime::raycast(
      *world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
      engine::math::Vec3(1.0F, 0.0F, 0.0F), 100.0F, &hit);
  if (!found) {
    return 124;
  }
  // Should hit the near entity (center at 3, near face at 2.5).
  if (hit.entity.index != near.index) {
    return 125;
  }
  if (std::fabs(hit.distance - 2.5F) > 0.1F) {
    return 126;
  }

  return 0;
}

int check_distance_joint_maintains_distance() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 130;
  }

  world->end_frame_phase();

  const engine::runtime::Entity anchor = world->create_entity();
  const engine::runtime::Entity ball = world->create_entity();
  if ((anchor == engine::runtime::kInvalidEntity) ||
      (ball == engine::runtime::kInvalidEntity)) {
    return 131;
  }

  engine::runtime::Transform anchorT{};
  anchorT.position = engine::math::Vec3(0.0F, 10.0F, 0.0F);
  engine::runtime::Transform ballT{};
  ballT.position = engine::math::Vec3(0.0F, 7.0F, 0.0F);

  engine::runtime::RigidBody anchorBody{};
  anchorBody.inverseMass = 0.0F; // static
  engine::runtime::RigidBody ballBody{};
  ballBody.inverseMass = 1.0F;

  engine::runtime::Collider col{};
  col.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);

  if (!world->add_transform(anchor, anchorT) ||
      !world->add_transform(ball, ballT)) {
    return 132;
  }
  if (!world->add_rigid_body(anchor, anchorBody) ||
      !world->add_rigid_body(ball, ballBody)) {
    return 133;
  }
  if (!world->add_collider(anchor, col) || !world->add_collider(ball, col)) {
    return 134;
  }

  // Add a distance joint of length 3.0 (current distance).
  const engine::physics::JointId jid =
      engine::runtime::add_distance_joint(*world, anchor, ball, 3.0F);
  if (jid == engine::physics::kInvalidJointId) {
    return 135;
  }

  // Run several physics steps with gravity pulling the ball down.
  for (int step = 0; step < 10; ++step) {
    world->begin_update_phase();
    if (!engine::runtime::step_physics(*world, 1.0F / 60.0F)) {
      world->end_frame_phase();
      return 136;
    }
    if (!engine::runtime::resolve_collisions(*world)) {
      world->end_frame_phase();
      return 137;
    }
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  engine::runtime::Transform outAnchor{};
  engine::runtime::Transform outBall{};
  if (!world->get_transform(anchor, &outAnchor) ||
      !world->get_transform(ball, &outBall)) {
    return 138;
  }

  const float dx = outBall.position.x - outAnchor.position.x;
  const float dy = outBall.position.y - outAnchor.position.y;
  const float dz = outBall.position.z - outAnchor.position.z;
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

  // Distance should be approximately 3.0 despite gravity.
  if (std::fabs(dist - 3.0F) > 0.5F) {
    return 139; // joint failed to maintain distance
  }

  engine::runtime::remove_joint(*world, jid);

  return 0;
}

int check_ccd_catches_fast_projectile() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 140;
  }

  world->end_frame_phase();

  // Thin wall at x=5.
  const engine::runtime::Entity wall = world->create_entity();
  // Fast projectile starting at x=0, moving at 1000 units/sec.
  const engine::runtime::Entity bullet = world->create_entity();
  if ((wall == engine::runtime::kInvalidEntity) ||
      (bullet == engine::runtime::kInvalidEntity)) {
    return 141;
  }

  engine::runtime::Transform wallT{};
  wallT.position = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  engine::runtime::Transform bulletT{};
  bulletT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);

  engine::runtime::Collider wallCol{};
  wallCol.halfExtents = engine::math::Vec3(0.05F, 2.0F, 2.0F); // very thin

  engine::runtime::Collider bulletCol{};
  bulletCol.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);

  engine::runtime::RigidBody wallBody{};
  wallBody.inverseMass = 0.0F; // static
  engine::runtime::RigidBody bulletBody{};
  bulletBody.inverseMass = 1.0F;
  bulletBody.velocity = engine::math::Vec3(1000.0F, 0.0F, 0.0F);

  if (!world->add_transform(wall, wallT) ||
      !world->add_transform(bullet, bulletT)) {
    return 142;
  }
  if (!world->add_collider(wall, wallCol) ||
      !world->add_collider(bullet, bulletCol)) {
    return 143;
  }
  if (!world->add_rigid_body(wall, wallBody) ||
      !world->add_rigid_body(bullet, bulletBody)) {
    return 144;
  }

  // Disable gravity so it doesn't interfere.
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  world->begin_update_phase();
  if (!engine::runtime::step_physics(*world, 1.0F / 60.0F)) {
    engine::runtime::set_gravity(*world, 0.0F, -9.8F, 0.0F);
    world->end_frame_phase();
    return 145;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::set_gravity(*world, 0.0F, -9.8F, 0.0F);

  engine::runtime::Transform outBullet{};
  if (!world->get_transform(bullet, &outBullet)) {
    return 146;
  }

  // Without CCD, bullet would be at x=16.67. With CCD, it should be
  // stopped near the wall (x < 5).
  if (outBullet.position.x > 5.0F) {
    return 147; // bullet tunneled through wall
  }

  return 0;
}

// --- Sleep tests ---

int check_body_falls_asleep() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 150;
  }

  world->end_frame_phase();

  // A ball resting on a static floor should fall asleep after enough frames.
  const engine::runtime::Entity floor = world->create_entity();
  const engine::runtime::Entity ball = world->create_entity();
  if ((floor == engine::runtime::kInvalidEntity) ||
      (ball == engine::runtime::kInvalidEntity)) {
    return 151;
  }

  engine::runtime::Transform floorT{};
  floorT.position = engine::math::Vec3(0.0F, -1.0F, 0.0F);
  engine::runtime::Transform ballT{};
  ballT.position = engine::math::Vec3(0.0F, 0.5F, 0.0F);

  engine::runtime::Collider floorCol{};
  floorCol.halfExtents = engine::math::Vec3(10.0F, 1.0F, 10.0F);
  floorCol.restitution = 0.0F;

  engine::runtime::Collider ballCol{};
  ballCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  ballCol.restitution = 0.0F;

  engine::runtime::RigidBody floorBody{};
  floorBody.inverseMass = 0.0F;
  engine::runtime::RigidBody ballBody{};
  ballBody.inverseMass = 1.0F;

  if (!world->add_transform(floor, floorT) ||
      !world->add_transform(ball, ballT)) {
    return 152;
  }
  if (!world->add_collider(floor, floorCol) ||
      !world->add_collider(ball, ballCol)) {
    return 153;
  }
  if (!world->add_rigid_body(floor, floorBody) ||
      !world->add_rigid_body(ball, ballBody)) {
    return 154;
  }

  // Step enough frames for the ball to settle and sleep (~120 frames).
  const float dt = 1.0F / 60.0F;
  for (int frame = 0; frame < 200; ++frame) {
    world->begin_update_phase();
    engine::runtime::step_physics(*world, dt);
    engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  if (!engine::runtime::is_sleeping(*world, ball)) {
    return 155; // ball should be sleeping
  }

  return 0;
}

int check_collision_wakes_body() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 160;
  }

  world->end_frame_phase();

  // A manually-sleeping ball and a fast projectile that hits it. No gravity.
  const engine::runtime::Entity ball = world->create_entity();
  const engine::runtime::Entity projectile = world->create_entity();
  if ((ball == engine::runtime::kInvalidEntity) ||
      (projectile == engine::runtime::kInvalidEntity)) {
    return 161;
  }

  engine::runtime::Transform ballT{};
  ballT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform projT{};
  projT.position = engine::math::Vec3(3.0F, 0.0F, 0.0F);

  engine::runtime::Collider ballCol{};
  ballCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::Collider projCol{};
  projCol.halfExtents = engine::math::Vec3(0.2F, 0.2F, 0.2F);

  engine::runtime::RigidBody ballBody{};
  ballBody.inverseMass = 1.0F;
  ballBody.sleeping = true; // Start sleeping.
  engine::runtime::RigidBody projBody{};
  projBody.inverseMass = 1.0F;
  projBody.velocity = engine::math::Vec3(-10.0F, 0.0F, 0.0F);

  if (!world->add_transform(ball, ballT) ||
      !world->add_transform(projectile, projT)) {
    return 162;
  }
  if (!world->add_collider(ball, ballCol) ||
      !world->add_collider(projectile, projCol)) {
    return 163;
  }
  if (!world->add_rigid_body(ball, ballBody) ||
      !world->add_rigid_body(projectile, projBody)) {
    return 164;
  }

  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  if (!engine::runtime::is_sleeping(*world, ball)) {
    engine::runtime::set_gravity(*world, 0.0F, -9.8F, 0.0F);
    return 165; // ball should start sleeping
  }

  const float dt = 1.0F / 60.0F;
  for (int frame = 0; frame < 30; ++frame) {
    world->begin_update_phase();
    engine::runtime::step_physics(*world, dt);
    engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  engine::runtime::set_gravity(*world, 0.0F, -9.8F, 0.0F);

  if (engine::runtime::is_sleeping(*world, ball)) {
    return 167; // ball should have been woken by collision
  }

  return 0;
}

int check_wake_body_api() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 170;
  }

  world->end_frame_phase();

  const engine::runtime::Entity ball = world->create_entity();
  if (ball == engine::runtime::kInvalidEntity) {
    return 171;
  }

  engine::runtime::Transform ballT{};
  ballT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);

  engine::runtime::Collider ballCol{};
  ballCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::RigidBody ballBody{};
  ballBody.inverseMass = 1.0F;

  if (!world->add_transform(ball, ballT)) {
    return 172;
  }
  if (!world->add_collider(ball, ballCol)) {
    return 173;
  }
  if (!world->add_rigid_body(ball, ballBody)) {
    return 174;
  }

  // Manually set sleeping.
  {
    engine::runtime::RigidBody *body = world->get_rigid_body_ptr(ball);
    if (body == nullptr) {
      return 175;
    }
    body->sleeping = true;
  }

  if (!engine::runtime::is_sleeping(*world, ball)) {
    return 176; // should report sleeping
  }

  engine::runtime::wake_body(*world, ball);

  if (engine::runtime::is_sleeping(*world, ball)) {
    return 177; // should be awake after wake_body
  }

  return 0;
}

int check_bridge_phase_misuse_rejected() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 180;
  }

  world->end_frame_phase();
  const engine::runtime::Entity a = world->create_entity();
  const engine::runtime::Entity b = world->create_entity();
  if ((a == engine::runtime::kInvalidEntity) ||
      (b == engine::runtime::kInvalidEntity)) {
    return 181;
  }

  engine::runtime::Transform t{};
  engine::runtime::RigidBody rb{};
  if (!world->add_transform(a, t) || !world->add_transform(b, t) ||
      !world->add_rigid_body(a, rb) || !world->add_rigid_body(b, rb)) {
    return 182;
  }

  if (engine::runtime::step_physics(*world, 1.0F / 60.0F)) {
    return 183;
  }

  world->begin_update_phase();
  const engine::physics::JointId badJoint =
      engine::runtime::add_distance_joint(*world, a, b, 1.0F);
  if (badJoint != engine::physics::kInvalidJointId) {
    world->end_frame_phase();
    return 184;
  }

  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);
  float gx = 0.0F;
  float gy = 0.0F;
  float gz = 0.0F;
  if (!engine::runtime::get_gravity(*world, &gx, &gy, &gz)) {
    world->end_frame_phase();
    return 185;
  }
  world->end_frame_phase();

  if (std::fabs(gx) > 1e-6F || std::fabs(gy + 9.8F) > 1e-6F ||
      std::fabs(gz) > 1e-6F) {
    return 186;
  }

  return 0;
}

int check_multi_world_physics_isolation() {
  std::unique_ptr<engine::runtime::World> worldA(new (std::nothrow)
                                                      engine::runtime::World());
  std::unique_ptr<engine::runtime::World> worldB(new (std::nothrow)
                                                      engine::runtime::World());
  if ((worldA == nullptr) || (worldB == nullptr)) {
    return 190;
  }

  worldA->end_frame_phase();
  worldB->end_frame_phase();

  const engine::runtime::Entity ea = worldA->create_entity();
  const engine::runtime::Entity eb = worldB->create_entity();
  if ((ea == engine::runtime::kInvalidEntity) ||
      (eb == engine::runtime::kInvalidEntity)) {
    return 191;
  }

  engine::runtime::Transform ta{};
  ta.position = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  engine::runtime::Transform tb{};
  tb.position = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  engine::runtime::RigidBody rb{};
  rb.inverseMass = 1.0F;

  if (!worldA->add_transform(ea, ta) || !worldB->add_transform(eb, tb) ||
      !worldA->add_rigid_body(ea, rb) || !worldB->add_rigid_body(eb, rb)) {
    return 192;
  }

  engine::runtime::set_gravity(*worldA, 0.0F, 0.0F, 0.0F);
  engine::runtime::set_gravity(*worldB, 0.0F, -20.0F, 0.0F);

  worldA->begin_update_phase();
  worldB->begin_update_phase();
  const bool stepA = engine::runtime::step_physics(*worldA, 1.0F / 60.0F);
  const bool stepB = engine::runtime::step_physics(*worldB, 1.0F / 60.0F);
  worldA->commit_update_phase();
  worldB->commit_update_phase();
  worldA->begin_render_prep_phase();
  worldB->begin_render_prep_phase();
  worldA->end_frame_phase();
  worldB->end_frame_phase();
  if (!stepA || !stepB) {
    return 193;
  }

  engine::runtime::Transform outA{};
  engine::runtime::Transform outB{};
  if (!worldA->get_transform(ea, &outA) || !worldB->get_transform(eb, &outB)) {
    return 194;
  }

  if (std::fabs(outA.position.y - 1.0F) > 0.001F) {
    return 195;
  }
  if (!(outB.position.y < 1.0F)) {
    return 196;
  }

  return 0;
}

int check_collision_bookkeeping_scale() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 200;
  }

  world->end_frame_phase();
  constexpr std::size_t kBodies = 96U;
  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;

  for (std::size_t i = 0U; i < kBodies; ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      return 201;
    }

    engine::runtime::Transform t{};
    t.position = engine::math::Vec3(static_cast<float>(i % 6U) * 0.1F,
                                    static_cast<float>(i / 6U) * 0.1F, 0.0F);
    if (!world->add_transform(entity, t) || !world->add_collider(entity, collider) ||
        !world->add_rigid_body(entity, body)) {
      return 202;
    }
  }

  engine::runtime::set_collision_dispatch(*world, &test_collision_dispatch);
  g_dispatchedPairCount = 0U;

  const auto start = std::chrono::steady_clock::now();
  world->begin_update_phase();
  const bool stepped = engine::runtime::step_physics(*world, 1.0F / 60.0F);
  const bool resolved = engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  engine::runtime::dispatch_collision_callbacks(*world);
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

  if (!stepped || !resolved) {
    return 203;
  }

  const std::size_t maxUniquePairs = (kBodies * (kBodies - 1U)) / 2U;
  if ((g_dispatchedPairCount == 0U) || (g_dispatchedPairCount > maxUniquePairs)) {
    return 204;
  }

  if (elapsedMs > 250) {
    return 205;
  }

  return 0;
}

} // namespace

int main() {
  int result = check_gravity_step();
  if (result != 0) {
    return result;
  }

  result = check_overlap_resolution();
  if (result != 0) {
    return result;
  }

  result = check_static_body_immovable();
  if (result != 0) {
    return result;
  }

  result = check_angular_velocity_integration();
  if (result != 0) {
    return result;
  }

  result = check_angular_impulse_from_collision();
  if (result != 0) {
    return result;
  }

  result = check_zero_inverse_inertia_prevents_rotation();
  if (result != 0) {
    return result;
  }

  result = check_high_restitution_bounce();
  if (result != 0) {
    return result;
  }

  result = check_zero_restitution_no_bounce();
  if (result != 0) {
    return result;
  }

  result = check_friction_slows_sliding();
  if (result != 0) {
    return result;
  }

  result = check_raycast_hits_aabb();
  if (result != 0) {
    return result;
  }

  result = check_raycast_hits_sphere();
  if (result != 0) {
    return result;
  }

  result = check_raycast_misses();
  if (result != 0) {
    return result;
  }

  result = check_raycast_returns_closest();
  if (result != 0) {
    return result;
  }

  result = check_distance_joint_maintains_distance();
  if (result != 0) {
    return result;
  }

  result = check_ccd_catches_fast_projectile();
  if (result != 0) {
    return result;
  }

  result = check_body_falls_asleep();
  if (result != 0) {
    return result;
  }

  result = check_collision_wakes_body();
  if (result != 0) {
    return result;
  }

  result = check_wake_body_api();
  if (result != 0) {
    return result;
  }

  result = check_bridge_phase_misuse_rejected();
  if (result != 0) {
    return result;
  }

  result = check_multi_world_physics_isolation();
  if (result != 0) {
    return result;
  }

  result = check_collision_bookkeeping_scale();
  if (result != 0) {
    return result;
  }

  return 0;
}
