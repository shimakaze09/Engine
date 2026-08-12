// Verifies physics test behavior for the Engine test suite.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/cvar.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/convex_hull.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/primitive_hulls.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

std::size_t g_dispatchedPairCount = 0U;
// Captured copy of the dispatched pair list (first kMaxCollisionPairs).
std::uint32_t g_dispatchedPairData[engine::physics::kMaxCollisionPairs * 2U];

void test_collision_dispatch(const std::uint32_t *pairs,
                             std::size_t pairCount) noexcept {
  g_dispatchedPairCount = pairCount;
  const std::size_t copyPairs = (pairCount < engine::physics::kMaxCollisionPairs)
                                    ? pairCount
                                    : engine::physics::kMaxCollisionPairs;
  std::memcpy(g_dispatchedPairData, pairs,
              copyPairs * 2U * sizeof(std::uint32_t));
}

int check_physics_cvars_register_after_core_cvars() {
  if (!engine::core::initialize_cvars()) {
    return 1000;
  }

  if (engine::core::cvar_get_int("physics.solver_iterations", -1) != -1) {
    engine::core::shutdown_cvars();
    return 1001;
  }
  if (!engine::physics::register_physics_cvars()) {
    engine::core::shutdown_cvars();
    return 1002;
  }
  if (engine::core::cvar_get_int("physics.solver_iterations", -1) != 8) {
    engine::core::shutdown_cvars();
    return 1003;
  }
  if (engine::core::cvar_get_float("physics.ccd_threshold", -1.0F) != 2.0F) {
    engine::core::shutdown_cvars();
    return 1004;
  }

  if (!engine::core::cvar_set_int("physics.solver_iterations", 12)) {
    engine::core::shutdown_cvars();
    return 1005;
  }
  if (!engine::physics::register_physics_cvars()) {
    engine::core::shutdown_cvars();
    return 1006;
  }
  if (engine::core::cvar_get_int("physics.solver_iterations", -1) != 12) {
    engine::core::shutdown_cvars();
    return 1007;
  }

  engine::core::CVarInfo infos[32] = {};
  const std::size_t count = engine::core::cvar_get_all(infos, 32U);
  std::size_t solverCount = 0U;
  std::size_t ccdCount = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    if ((infos[i].name != nullptr) &&
        (std::strcmp(infos[i].name, "physics.solver_iterations") == 0)) {
      ++solverCount;
    }
    if ((infos[i].name != nullptr) &&
        (std::strcmp(infos[i].name, "physics.ccd_threshold") == 0)) {
      ++ccdCount;
    }
  }
  if ((solverCount != 1U) || (ccdCount != 1U)) {
    engine::core::shutdown_cvars();
    return 1008;
  }

  engine::core::shutdown_cvars();
  return 0;
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

int run_speculative_timestep_case(float deltaSeconds, float *outVelocityX) {
  if (outVelocityX == nullptr) {
    return 1010;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1011;
  }

  world->end_frame_phase();

  const engine::runtime::Entity moving = world->create_entity();
  const engine::runtime::Entity wall = world->create_entity();
  if ((moving == engine::runtime::kInvalidEntity) ||
      (wall == engine::runtime::kInvalidEntity)) {
    return 1012;
  }

  engine::runtime::Transform movingT{};
  movingT.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform wallT{};
  wallT.position = engine::math::Vec3(1.2F, 0.0F, 0.0F);

  engine::runtime::Collider box{};
  box.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  box.restitution = 0.0F;

  engine::runtime::RigidBody movingBody{};
  movingBody.inverseMass = 1.0F;
  movingBody.velocity = engine::math::Vec3(10.0F, 0.0F, 0.0F);
  engine::runtime::RigidBody wallBody{};
  wallBody.inverseMass = 0.0F;

  if (!world->add_transform(moving, movingT) ||
      !world->add_transform(wall, wallT)) {
    return 1013;
  }
  if (!world->add_collider(moving, box) || !world->add_collider(wall, box)) {
    return 1014;
  }
  if (!world->add_rigid_body(moving, movingBody) ||
      !world->add_rigid_body(wall, wallBody)) {
    return 1015;
  }

  world->begin_update_phase();
  if (!engine::runtime::resolve_collisions(*world, deltaSeconds)) {
    world->end_frame_phase();
    return 1016;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::RigidBody outBody{};
  if (!world->get_rigid_body(moving, &outBody)) {
    return 1017;
  }

  *outVelocityX = outBody.velocity.x;
  return 0;
}

int check_resolve_collisions_uses_delta_seconds() {
  float shortDtVelocity = 0.0F;
  int result = run_speculative_timestep_case(0.01F, &shortDtVelocity);
  if (result != 0) {
    return result;
  }

  float longDtVelocity = 0.0F;
  result = run_speculative_timestep_case(0.1F, &longDtVelocity);
  if (result != 0) {
    return result;
  }

  if (std::fabs(shortDtVelocity - 10.0F) > 0.001F) {
    return 1018;
  }
  if (!(longDtVelocity < 9.0F)) {
    return 1019;
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
    if (!world->add_transform(entity, t) ||
        !world->add_collider(entity, collider) ||
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
  const double elapsedMs =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start)
          .count();

  if (!stepped || !resolved) {
    return 203;
  }

  // Snapshot bookkeeping before dispatch drains the frame accumulators.
  const engine::physics::PhysicsContext &ctx = world->physics_context();
  const std::size_t keptPairs = ctx.collisionPairCount;
  const std::size_t framePairs = ctx.frameCollisionPairCount;
  const std::uint32_t stepDrops = ctx.collisionPairDropCount;
  const std::uint32_t frameDrops = ctx.frameCollisionPairDropCount;
  engine::runtime::dispatch_collision_callbacks(*world);

  // Diagnostic only: wall-clock budgets live in engine_bench_physics_perf.
  std::printf("[collision_bookkeeping] kept=%zu dropped=%u step_ms=%.3f\n",
              keptPairs, stepDrops, elapsedMs);

  if (keptPairs != engine::physics::kMaxCollisionPairs) {
    return 204;
  }
  if ((framePairs != keptPairs) || (frameDrops != stepDrops)) {
    return 205;
  }

  // Bodies within nine 0.1-spaced rows must overlap (1.0 extent sum), so at
  // least 3804 of the 4560 unique pairs collide and the cap must overflow.
  constexpr std::size_t kGuaranteedOverlapPairs = 3804U;
  const std::size_t maxUniquePairs = (kBodies * (kBodies - 1U)) / 2U;
  const std::size_t detectedPairs =
      keptPairs + static_cast<std::size_t>(stepDrops);
  if ((detectedPairs < kGuaranteedOverlapPairs) ||
      (detectedPairs > maxUniquePairs)) {
    return 206;
  }

  if ((ctx.collisionPairOverflowEpisodes != 1U) ||
      !ctx.collisionPairOverflowActive) {
    return 207;
  }
  if (ctx.broadphaseOverflowEpisodes != 0U) {
    return 208;
  }

  if (g_dispatchedPairCount != keptPairs) {
    return 209;
  }

  // Dispatched pairs must reference valid, distinct bodies with no
  // duplicate unordered pair surviving the dedupe hash.
  bool seen[kBodies + 1U][kBodies + 1U] = {};
  for (std::size_t i = 0U; i < g_dispatchedPairCount; ++i) {
    const std::uint32_t a = g_dispatchedPairData[i * 2U];
    const std::uint32_t b = g_dispatchedPairData[(i * 2U) + 1U];
    if ((a < 1U) || (a > kBodies) || (b < 1U) || (b > kBodies) || (a == b)) {
      return 210;
    }
    const std::uint32_t lo = (a < b) ? a : b;
    const std::uint32_t hi = (a < b) ? b : a;
    if (seen[lo][hi]) {
      return 211;
    }
    seen[lo][hi] = true;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Capsule-vs-AABB collision: two overlapping entities should be separated.
// ---------------------------------------------------------------------------
int check_capsule_vs_aabb_collision() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 300;
  }

  world->end_frame_phase();

  const engine::runtime::Entity cap = world->create_entity();
  const engine::runtime::Entity box = world->create_entity();
  if ((cap == engine::runtime::kInvalidEntity) ||
      (box == engine::runtime::kInvalidEntity)) {
    return 301;
  }

  engine::runtime::Transform tCap{};
  tCap.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tBox{};
  tBox.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);

  engine::runtime::Collider capCol{};
  capCol.shape = engine::runtime::ColliderShape::Capsule;
  capCol.halfExtents = engine::math::Vec3(0.3F, 0.5F, 0.3F); // r=0.3, hh=0.5

  engine::runtime::Collider boxCol{};
  boxCol.shape = engine::runtime::ColliderShape::AABB;
  boxCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;

  if (!world->add_transform(cap, tCap) || !world->add_transform(box, tBox)) {
    return 302;
  }
  if (!world->add_collider(cap, capCol) || !world->add_collider(box, boxCol)) {
    return 303;
  }
  if (!world->add_rigid_body(cap, body) || !world->add_rigid_body(box, body)) {
    return 304;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 305;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 306;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform outCap{};
  engine::runtime::Transform outBox{};
  if (!world->get_transform(cap, &outCap) ||
      !world->get_transform(box, &outBox)) {
    return 307;
  }

  // They should have been pushed apart.
  const float separation = std::fabs(outBox.position.x - outCap.position.x) +
                           std::fabs(outBox.position.y - outCap.position.y) +
                           std::fabs(outBox.position.z - outCap.position.z);
  if (separation < 0.1F) {
    return 308;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Capsule-vs-Sphere collision: overlapping capsule and sphere are separated.
// ---------------------------------------------------------------------------
int check_capsule_vs_sphere_collision() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 310;
  }

  world->end_frame_phase();

  const engine::runtime::Entity cap = world->create_entity();
  const engine::runtime::Entity sph = world->create_entity();
  if ((cap == engine::runtime::kInvalidEntity) ||
      (sph == engine::runtime::kInvalidEntity)) {
    return 311;
  }

  engine::runtime::Transform tCap{};
  tCap.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tSph{};
  tSph.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);

  engine::runtime::Collider capCol{};
  capCol.shape = engine::runtime::ColliderShape::Capsule;
  capCol.halfExtents = engine::math::Vec3(0.3F, 0.5F, 0.3F); // r=0.3, hh=0.5

  engine::runtime::Collider sphCol{};
  sphCol.shape = engine::runtime::ColliderShape::Sphere;
  sphCol.halfExtents = engine::math::Vec3(0.4F, 0.4F, 0.4F); // r=0.4

  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;

  if (!world->add_transform(cap, tCap) || !world->add_transform(sph, tSph)) {
    return 312;
  }
  if (!world->add_collider(cap, capCol) || !world->add_collider(sph, sphCol)) {
    return 313;
  }
  if (!world->add_rigid_body(cap, body) || !world->add_rigid_body(sph, body)) {
    return 314;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 315;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 316;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform outCap{};
  engine::runtime::Transform outSph{};
  if (!world->get_transform(cap, &outCap) ||
      !world->get_transform(sph, &outSph)) {
    return 317;
  }

  // They should have been pushed apart.
  const float dxCS = std::fabs(outSph.position.x - outCap.position.x) +
                     std::fabs(outSph.position.y - outCap.position.y) +
                     std::fabs(outSph.position.z - outCap.position.z);
  if (dxCS < 0.1F) {
    return 318;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Capsule-vs-Capsule collision: two overlapping capsules are separated.
// ---------------------------------------------------------------------------
int check_capsule_vs_capsule_collision() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 320;
  }

  world->end_frame_phase();

  const engine::runtime::Entity a = world->create_entity();
  const engine::runtime::Entity b = world->create_entity();
  if ((a == engine::runtime::kInvalidEntity) ||
      (b == engine::runtime::kInvalidEntity)) {
    return 321;
  }

  engine::runtime::Transform tA{};
  tA.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);

  engine::runtime::Collider capCol{};
  capCol.shape = engine::runtime::ColliderShape::Capsule;
  capCol.halfExtents = engine::math::Vec3(0.3F, 0.5F, 0.3F); // r=0.3, hh=0.5

  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;

  if (!world->add_transform(a, tA) || !world->add_transform(b, tB)) {
    return 322;
  }
  if (!world->add_collider(a, capCol) || !world->add_collider(b, capCol)) {
    return 323;
  }
  if (!world->add_rigid_body(a, body) || !world->add_rigid_body(b, body)) {
    return 324;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 325;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 326;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform outA{};
  engine::runtime::Transform outB{};
  if (!world->get_transform(a, &outA) || !world->get_transform(b, &outB)) {
    return 327;
  }

  // They should have been pushed apart.
  const float dxCC = std::fabs(outB.position.x - outA.position.x) +
                     std::fabs(outB.position.y - outA.position.y) +
                     std::fabs(outB.position.z - outA.position.z);
  if (dxCC < 0.1F) {
    return 328;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Raycast hits a capsule.
// ---------------------------------------------------------------------------
int check_raycast_hits_capsule() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 330;
  }

  world->end_frame_phase();

  const engine::runtime::Entity cap = world->create_entity();
  if (cap == engine::runtime::kInvalidEntity) {
    return 331;
  }

  engine::runtime::Transform t{};
  t.position = engine::math::Vec3(0.0F, 0.0F, 10.0F);
  if (!world->add_transform(cap, t)) {
    return 332;
  }

  engine::runtime::Collider col{};
  col.shape = engine::runtime::ColliderShape::Capsule;
  col.halfExtents = engine::math::Vec3(1.0F, 2.0F, 1.0F); // r=1, hh=2
  if (!world->add_collider(cap, col)) {
    return 333;
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
    return 334;
  }
  // Capsule at z=10 with radius 1 → near surface at z=9 (cylinder part).
  if (std::fabs(hit.distance - 9.0F) > 0.15F) {
    return 335;
  }
  // Normal should point toward ray origin: (0, 0, -1) approximately.
  if (hit.normal.z > -0.9F) {
    return 336;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Build convex hull from cube vertices.
// ---------------------------------------------------------------------------
int check_convex_hull_build() {
  // Unit cube centered at origin: 8 vertices.
  engine::math::Vec3 cubeVerts[8] = {
      engine::math::Vec3(-0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, 0.5F, 0.5F),
      engine::math::Vec3(-0.5F, 0.5F, 0.5F),
  };

  engine::physics::ConvexHullData hull{};
  if (!engine::physics::build_convex_hull(cubeVerts, 8U, hull)) {
    return 400;
  }

  // Cube should produce 6 planes (one per face) — or 12 triangulated faces.
  // Quickhull triangulates, so we expect 12 triangle faces for a cube.
  if (hull.planeCount < 6U || hull.planeCount > 12U) {
    return 401;
  }

  // Should have 8 hull vertices.
  if (hull.vertexCount != 8U) {
    return 402;
  }

  // Local AABB half-extents should be ~(0.5, 0.5, 0.5).
  if (std::fabs(hull.localHalfExtents.x - 0.5F) > 0.05F ||
      std::fabs(hull.localHalfExtents.y - 0.5F) > 0.05F ||
      std::fabs(hull.localHalfExtents.z - 0.5F) > 0.05F) {
    return 403;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// ConvexHull vs AABB collision: overlapping are separated by physics.
// ---------------------------------------------------------------------------
int check_convex_hull_vs_aabb_collision() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 410;
  }

  engine::math::Vec3 cubeVerts[8] = {
      engine::math::Vec3(-0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, 0.5F, 0.5F),
      engine::math::Vec3(-0.5F, 0.5F, 0.5F),
  };
  engine::physics::ConvexHullData hull{};
  if (!engine::physics::build_convex_hull(cubeVerts, 8U, hull)) {
    return 411;
  }

  world->end_frame_phase();

  const engine::runtime::Entity hullEnt = world->create_entity();
  const engine::runtime::Entity boxEnt = world->create_entity();
  if ((hullEnt == engine::runtime::kInvalidEntity) ||
      (boxEnt == engine::runtime::kInvalidEntity)) {
    return 412;
  }

  engine::runtime::Transform tH{};
  tH.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);

  engine::runtime::Collider hullCol{};
  hullCol.shape = engine::runtime::ColliderShape::ConvexHull;
  hullCol.halfExtents = hull.localHalfExtents;

  engine::runtime::Collider boxCol{};
  boxCol.shape = engine::runtime::ColliderShape::AABB;
  boxCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;

  if (!world->add_transform(hullEnt, tH) || !world->add_transform(boxEnt, tB)) {
    return 413;
  }
  if (!world->add_collider(hullEnt, hullCol) ||
      !world->add_collider(boxEnt, boxCol)) {
    return 414;
  }
  if (!world->add_rigid_body(hullEnt, body) ||
      !world->add_rigid_body(boxEnt, body)) {
    return 415;
  }

  // Assign hull data to the entity.
  if (!engine::runtime::set_convex_hull_data(*world, hullEnt, hull)) {
    return 416;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 417;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 418;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform outH{};
  engine::runtime::Transform outB{};
  if (!world->get_transform(hullEnt, &outH) ||
      !world->get_transform(boxEnt, &outB)) {
    return 419;
  }

  // They should have been pushed apart.
  const float separation = std::fabs(outB.position.x - outH.position.x) +
                           std::fabs(outB.position.y - outH.position.y) +
                           std::fabs(outB.position.z - outH.position.z);
  if (separation < 0.1F) {
    return 420;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Raycast hits a convex hull.
// ---------------------------------------------------------------------------
int check_raycast_hits_convex_hull() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 430;
  }

  engine::math::Vec3 cubeVerts[8] = {
      engine::math::Vec3(-0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, 0.5F, 0.5F),
      engine::math::Vec3(-0.5F, 0.5F, 0.5F),
  };
  engine::physics::ConvexHullData hull{};
  if (!engine::physics::build_convex_hull(cubeVerts, 8U, hull)) {
    return 431;
  }

  world->end_frame_phase();

  const engine::runtime::Entity ent = world->create_entity();
  if (ent == engine::runtime::kInvalidEntity) {
    return 432;
  }

  engine::runtime::Transform t{};
  t.position = engine::math::Vec3(0.0F, 0.0F, 10.0F);
  if (!world->add_transform(ent, t)) {
    return 433;
  }

  engine::runtime::Collider col{};
  col.shape = engine::runtime::ColliderShape::ConvexHull;
  col.halfExtents = hull.localHalfExtents;
  if (!world->add_collider(ent, col)) {
    return 434;
  }

  if (!engine::runtime::set_convex_hull_data(*world, ent, hull)) {
    return 435;
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
    return 436;
  }
  // Hull at z=10, half-extent 0.5 → near face at z=9.5.
  if (std::fabs(hit.distance - 9.5F) > 0.15F) {
    return 437;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Heightfield collision: sphere rests on flat heightfield terrain.
// ---------------------------------------------------------------------------
int check_heightfield_vs_sphere_collision() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 440;
  }

  // Build a flat heightfield at y=0 (all samples = 0).
  engine::physics::HeightfieldData hf{};
  hf.rows = 5U;
  hf.columns = 5U;
  hf.spacingX = 2.0F;
  hf.spacingZ = 2.0F;
  hf.minY = 0.0F;
  hf.maxY = 0.0F;
  // All heights default to 0.

  world->end_frame_phase();

  const engine::runtime::Entity terrain = world->create_entity();
  const engine::runtime::Entity sphere = world->create_entity();
  if ((terrain == engine::runtime::kInvalidEntity) ||
      (sphere == engine::runtime::kInvalidEntity)) {
    return 441;
  }

  // Terrain at origin.
  engine::runtime::Transform tTerrain{};
  tTerrain.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  // Sphere slightly below terrain surface (y=-0.3, radius=0.5 → bottom at
  // -0.8).
  engine::runtime::Transform tSphere{};
  tSphere.position = engine::math::Vec3(0.0F, -0.3F, 0.0F);

  // Heightfield collider: half extents cover the terrain footprint.
  const float totalW = static_cast<float>(hf.columns - 1U) * hf.spacingX;
  const float totalD = static_cast<float>(hf.rows - 1U) * hf.spacingZ;
  engine::runtime::Collider terrainCol{};
  terrainCol.shape = engine::runtime::ColliderShape::Heightfield;
  terrainCol.halfExtents =
      engine::math::Vec3(totalW * 0.5F, 1.0F, totalD * 0.5F);

  engine::runtime::Collider sphCol{};
  sphCol.shape = engine::runtime::ColliderShape::Sphere;
  sphCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;

  // Terrain is static (no rigid body).
  if (!world->add_transform(terrain, tTerrain) ||
      !world->add_transform(sphere, tSphere)) {
    return 442;
  }
  if (!world->add_collider(terrain, terrainCol) ||
      !world->add_collider(sphere, sphCol)) {
    return 443;
  }
  if (!world->add_rigid_body(sphere, body)) {
    return 444;
  }

  if (!engine::runtime::set_heightfield_data(*world, terrain, hf)) {
    return 445;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 446;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 447;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform outSph{};
  if (!world->get_transform(sphere, &outSph)) {
    return 448;
  }

  // Sphere should have been pushed up.  Its center should be at least at
  // y >= radius (0.5) minus some tolerance.
  if (outSph.position.y < 0.3F) {
    return 449;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Heightfield: object wholly outside the grid on the negative side.
// ---------------------------------------------------------------------------

/// EXPECTATION: with declared collider extents wider than the payload
/// grid, an object entirely in negative grid coordinates produces no
/// contact and resolve completes — the cell bounds must clamp in signed
/// space instead of wrapping through size_t (audit H-04).
int check_heightfield_object_outside_grid_negative() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 3400;
  }

  engine::physics::HeightfieldData hf{};
  hf.rows = 5U;
  hf.columns = 5U;
  hf.spacingX = 2.0F;
  hf.spacingZ = 2.0F;
  hf.minY = 0.0F;
  hf.maxY = 0.0F;

  world->end_frame_phase();

  const engine::runtime::Entity terrain = world->create_entity();
  const engine::runtime::Entity sphere = world->create_entity();
  if ((terrain == engine::runtime::kInvalidEntity) ||
      (sphere == engine::runtime::kInvalidEntity)) {
    return 3401;
  }

  engine::runtime::Transform tTerrain{};
  engine::runtime::Transform tSphere{};
  tSphere.position = engine::math::Vec3(-50.0F, -0.3F, -50.0F);

  engine::runtime::Collider terrainCol{};
  terrainCol.shape = engine::runtime::ColliderShape::Heightfield;
  terrainCol.halfExtents = engine::math::Vec3(100.0F, 1.0F, 100.0F);

  engine::runtime::Collider sphCol{};
  sphCol.shape = engine::runtime::ColliderShape::Sphere;
  sphCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);

  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;

  if (!world->add_transform(terrain, tTerrain) ||
      !world->add_transform(sphere, tSphere) ||
      !world->add_collider(terrain, terrainCol) ||
      !world->add_collider(sphere, sphCol) ||
      !world->add_rigid_body(sphere, body)) {
    return 3402;
  }
  if (!engine::runtime::set_heightfield_data(*world, terrain, hf)) {
    return 3403;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(), 0.0F)) {
    world->end_frame_phase();
    return 3404;
  }
  if (!engine::runtime::resolve_collisions(*world)) {
    world->end_frame_phase();
    return 3405;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform outSph{};
  if (!world->get_transform(sphere, &outSph)) {
    return 3406;
  }
  if ((outSph.position.x != -50.0F) || (outSph.position.y != -0.3F) ||
      (outSph.position.z != -50.0F)) {
    return 3407;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Raycast hits a flat heightfield.
// ---------------------------------------------------------------------------
int check_raycast_hits_heightfield() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 450;
  }

  // Flat heightfield at y=0.
  engine::physics::HeightfieldData hf{};
  hf.rows = 5U;
  hf.columns = 5U;
  hf.spacingX = 2.0F;
  hf.spacingZ = 2.0F;
  hf.minY = 0.0F;
  hf.maxY = 0.0F;

  world->end_frame_phase();

  const engine::runtime::Entity terrain = world->create_entity();
  if (terrain == engine::runtime::kInvalidEntity) {
    return 451;
  }

  engine::runtime::Transform t{};
  t.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  if (!world->add_transform(terrain, t)) {
    return 452;
  }

  const float totalW = static_cast<float>(hf.columns - 1U) * hf.spacingX;
  const float totalD = static_cast<float>(hf.rows - 1U) * hf.spacingZ;
  engine::runtime::Collider terrainCol{};
  terrainCol.shape = engine::runtime::ColliderShape::Heightfield;
  terrainCol.halfExtents =
      engine::math::Vec3(totalW * 0.5F, 1.0F, totalD * 0.5F);
  if (!world->add_collider(terrain, terrainCol)) {
    return 453;
  }
  if (!engine::runtime::set_heightfield_data(*world, terrain, hf)) {
    return 454;
  }

  world->begin_update_phase();
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  // Cast ray straight down from y=10.
  engine::runtime::PhysicsRaycastHit hit{};
  const bool found = engine::runtime::raycast(
      *world, engine::math::Vec3(0.0F, 10.0F, 0.0F),
      engine::math::Vec3(0.0F, -1.0F, 0.0F), 100.0F, &hit);
  if (!found) {
    return 455;
  }
  // Should hit at y=0, so distance ≈ 10.
  if (std::fabs(hit.distance - 10.0F) > 0.5F) {
    return 456;
  }
  // Normal should point up.
  if (hit.normal.y < 0.9F) {
    return 457;
  }

  return 0;
}

/// Checks non-primitive collider payloads stay scoped to their World.
int check_shape_payload_world_isolation() {
  std::unique_ptr<engine::runtime::World> worldA(new (std::nothrow)
                                                     engine::runtime::World());
  std::unique_ptr<engine::runtime::World> worldB(new (std::nothrow)
                                                     engine::runtime::World());
  if ((worldA == nullptr) || (worldB == nullptr)) {
    return 500;
  }

  worldA->end_frame_phase();
  worldB->end_frame_phase();

  const engine::runtime::Entity entityA = worldA->create_entity();
  const engine::runtime::Entity entityB = worldB->create_entity();
  if ((entityA == engine::runtime::kInvalidEntity) ||
      (entityB == engine::runtime::kInvalidEntity)) {
    return 501;
  }
  if (entityA.index != entityB.index) {
    return 502;
  }

  engine::math::Vec3 cubeVerts[8] = {
      engine::math::Vec3(-0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, 0.5F, 0.5F),
      engine::math::Vec3(-0.5F, 0.5F, 0.5F),
  };
  engine::physics::ConvexHullData hull{};
  if (!engine::physics::build_convex_hull(cubeVerts, 8U, hull)) {
    return 503;
  }
  if (!engine::runtime::set_convex_hull_data(*worldA, entityA, hull)) {
    return 504;
  }
  if (engine::runtime::get_convex_hull_data(*worldA, entityA) == nullptr) {
    return 505;
  }
  if (engine::runtime::get_convex_hull_data(*worldB, entityB) != nullptr) {
    return 506;
  }

  engine::physics::HeightfieldData hf{};
  hf.rows = 2U;
  hf.columns = 2U;
  hf.spacingX = 1.0F;
  hf.spacingZ = 1.0F;
  if (!engine::runtime::set_heightfield_data(*worldB, entityB, hf)) {
    return 507;
  }
  if (engine::runtime::get_heightfield_data(*worldB, entityB) == nullptr) {
    return 508;
  }
  if (engine::runtime::get_heightfield_data(*worldA, entityA) != nullptr) {
    return 509;
  }

  return 0;
}

int check_shape_payloads_do_not_survive_entity_reuse() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 540;
  }
  world->end_frame_phase();

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 541;
  }

  engine::math::Vec3 cubeVerts[8] = {
      engine::math::Vec3(-0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, 0.5F, 0.5F),
      engine::math::Vec3(-0.5F, 0.5F, 0.5F),
  };
  engine::physics::ConvexHullData hull{};
  if (!engine::physics::build_convex_hull(cubeVerts, 8U, hull)) {
    return 542;
  }
  if (!engine::runtime::set_convex_hull_data(*world, entity, hull)) {
    return 543;
  }

  engine::physics::HeightfieldData heightfield{};
  heightfield.rows = 2U;
  heightfield.columns = 2U;
  heightfield.spacingX = 1.0F;
  heightfield.spacingZ = 1.0F;
  if (!engine::runtime::set_heightfield_data(*world, entity, heightfield)) {
    return 544;
  }

  if (!world->destroy_entity(entity)) {
    return 545;
  }
  if (engine::runtime::get_convex_hull_data(*world, entity) != nullptr) {
    return 546;
  }
  if (engine::runtime::get_heightfield_data(*world, entity) != nullptr) {
    return 547;
  }
  if (engine::runtime::set_convex_hull_data(*world, entity, hull)) {
    return 548;
  }
  if (engine::runtime::set_heightfield_data(*world, entity, heightfield)) {
    return 549;
  }

  const engine::runtime::Entity reused = world->create_entity();
  if (reused == engine::runtime::kInvalidEntity) {
    return 550;
  }
  if (reused.index != entity.index) {
    return 551;
  }
  if (reused.generation == entity.generation) {
    return 552;
  }
  if (engine::runtime::get_convex_hull_data(*world, reused) != nullptr) {
    return 553;
  }
  if (engine::runtime::get_heightfield_data(*world, reused) != nullptr) {
    return 554;
  }

  return 0;
}

/// Replacing a collider's shape must drop the payload the new shape cannot
/// consume, and the payload setters must honor the Input-phase mutation rule.
int check_collider_replacement_prunes_payloads() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 560;
  }
  world->end_frame_phase();
  const engine::runtime::Entity entity = world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return 561;
  }

  engine::runtime::Collider heightfieldCollider{};
  heightfieldCollider.shape = engine::runtime::ColliderShape::Heightfield;
  if (!world->add_collider(entity, heightfieldCollider)) {
    return 562;
  }
  engine::physics::HeightfieldData heightfield{};
  heightfield.rows = 2U;
  heightfield.columns = 2U;
  heightfield.spacingX = 1.0F;
  heightfield.spacingZ = 1.0F;
  if (!engine::runtime::set_heightfield_data(*world, entity, heightfield)) {
    return 563;
  }
  if (engine::runtime::get_heightfield_data(*world, entity) == nullptr) {
    return 564;
  }

  engine::runtime::Collider boxCollider{};
  boxCollider.shape = engine::runtime::ColliderShape::AABB;
  if (!world->add_collider(entity, boxCollider)) {
    return 565;
  }
  if (engine::runtime::get_heightfield_data(*world, entity) != nullptr) {
    return 566;
  }

  engine::runtime::Collider hullCollider{};
  hullCollider.shape = engine::runtime::ColliderShape::ConvexHull;
  hullCollider.hullSource = engine::runtime::HullSource::Cylinder;
  if (!world->add_collider(entity, hullCollider)) {
    return 567;
  }
  if (engine::runtime::get_convex_hull_data(*world, entity) == nullptr) {
    return 568;
  }
  if (!world->add_collider(entity, boxCollider)) {
    return 569;
  }
  if (engine::runtime::get_convex_hull_data(*world, entity) != nullptr) {
    return 570;
  }

  world->begin_transform_phase();
  engine::physics::HeightfieldData rejected{};
  rejected.rows = 2U;
  rejected.columns = 2U;
  rejected.spacingX = 1.0F;
  rejected.spacingZ = 1.0F;
  if (engine::runtime::set_heightfield_data(*world, entity, rejected)) {
    return 571;
  }
  world->end_frame_phase();
  return 0;
}

int check_invalid_shape_payloads_rejected() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 520;
  }
  world->end_frame_phase();
  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 521;
  }

  engine::physics::ConvexHullData invalidHull{};
  if (engine::runtime::set_convex_hull_data(*world, entity, invalidHull)) {
    return 522;
  }
  if (engine::runtime::get_convex_hull_data(*world, entity) != nullptr) {
    return 523;
  }

  invalidHull.vertexCount = engine::physics::ConvexHullData::kMaxVertices + 1U;
  invalidHull.planeCount = 1U;
  if (engine::runtime::set_convex_hull_data(*world, entity, invalidHull)) {
    return 524;
  }

  engine::math::Vec3 cubeVerts[8] = {
      engine::math::Vec3(-0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, -0.5F, -0.5F),
      engine::math::Vec3(0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, 0.5F, -0.5F),
      engine::math::Vec3(-0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, -0.5F, 0.5F),
      engine::math::Vec3(0.5F, 0.5F, 0.5F),
      engine::math::Vec3(-0.5F, 0.5F, 0.5F),
  };
  engine::physics::ConvexHullData validHull{};
  if (!engine::physics::build_convex_hull(cubeVerts, 8U, validHull)) {
    return 525;
  }
  if (!engine::runtime::set_convex_hull_data(*world, entity, validHull)) {
    return 526;
  }
  invalidHull = validHull;
  invalidHull.planeCount = engine::physics::ConvexHullData::kMaxPlanes + 1U;
  if (engine::runtime::set_convex_hull_data(*world, entity, invalidHull)) {
    return 527;
  }
  if (engine::runtime::get_convex_hull_data(*world, entity) == nullptr) {
    return 528;
  }

  engine::physics::HeightfieldData invalidHeightfield{};
  invalidHeightfield.rows = 1U;
  invalidHeightfield.columns = 2U;
  if (engine::runtime::set_heightfield_data(*world, entity,
                                            invalidHeightfield)) {
    return 529;
  }
  if (engine::runtime::get_heightfield_data(*world, entity) != nullptr) {
    return 530;
  }

  invalidHeightfield.rows = 2U;
  invalidHeightfield.columns =
      engine::physics::HeightfieldData::kMaxResolution + 1U;
  if (engine::runtime::set_heightfield_data(*world, entity,
                                            invalidHeightfield)) {
    return 531;
  }

  invalidHeightfield.columns = 2U;
  invalidHeightfield.spacingX = 0.0F;
  if (engine::runtime::set_heightfield_data(*world, entity,
                                            invalidHeightfield)) {
    return 532;
  }

  engine::physics::HeightfieldData validHeightfield{};
  validHeightfield.rows = 2U;
  validHeightfield.columns = 2U;
  validHeightfield.spacingX = 1.0F;
  validHeightfield.spacingZ = 1.0F;
  if (!engine::runtime::set_heightfield_data(*world, entity,
                                             validHeightfield)) {
    return 533;
  }
  invalidHeightfield = validHeightfield;
  invalidHeightfield.spacingZ = -1.0F;
  if (engine::runtime::set_heightfield_data(*world, entity,
                                            invalidHeightfield)) {
    return 534;
  }
  if (engine::runtime::get_heightfield_data(*world, entity) == nullptr) {
    return 535;
  }

  return 0;
}

// The built-in cylinder and pyramid primitives collide as convex hulls that
// match their meshes: a cylinder has a flat top (not a capsule dome) and a
// pyramid's empty corners do not collide (not a bounding box).
int check_primitive_hull_collision() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 900;
  }
  world->end_frame_phase();

  engine::physics::ConvexHullData cylinderHull{};
  engine::physics::ConvexHullData pyramidHull{};
  if (!engine::physics::build_cylinder_hull(&cylinderHull) ||
      !engine::physics::build_pyramid_hull(&pyramidHull)) {
    return 901;
  }

  const engine::runtime::Entity cylinder = world->create_scene_object();
  engine::runtime::Transform pyramidTransform{};
  pyramidTransform.position = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  const engine::runtime::Entity pyramid =
      world->create_scene_object(pyramidTransform);
  if ((cylinder == engine::runtime::kInvalidEntity) ||
      (pyramid == engine::runtime::kInvalidEntity)) {
    return 902;
  }

  engine::runtime::Collider cylinderCollider{};
  cylinderCollider.shape = engine::runtime::ColliderShape::ConvexHull;
  cylinderCollider.halfExtents = cylinderHull.localHalfExtents;
  engine::runtime::Collider pyramidCollider{};
  pyramidCollider.shape = engine::runtime::ColliderShape::ConvexHull;
  pyramidCollider.halfExtents = pyramidHull.localHalfExtents;
  if (!world->add_collider(cylinder, cylinderCollider) ||
      !world->add_collider(pyramid, pyramidCollider) ||
      !engine::runtime::set_convex_hull_data(*world, cylinder, cylinderHull) ||
      !engine::runtime::set_convex_hull_data(*world, pyramid, pyramidHull)) {
    return 903;
  }

  // Flat cylinder top: a ray down the axis hits at exactly y = 0.5 (an
  // upright capsule of the same footprint would answer y = 1.0).
  engine::runtime::PhysicsRaycastHit hit{};
  if (!engine::runtime::raycast(*world, engine::math::Vec3(0.0F, 10.0F, 0.0F),
                                engine::math::Vec3(0.0F, -1.0F, 0.0F), 100.0F,
                                &hit)) {
    return 904;
  }
  if ((hit.entity != cylinder) || (hit.distance != 9.5F) ||
      (hit.point.y != 0.5F)) {
    return 905;
  }

  // Pyramid apex: the slant planes intersect the axis ray at exactly 0.5.
  engine::runtime::PhysicsRaycastHit apexHit{};
  if (!engine::runtime::raycast(*world, engine::math::Vec3(5.0F, 10.0F, 0.0F),
                                engine::math::Vec3(0.0F, -1.0F, 0.0F), 100.0F,
                                &apexHit)) {
    return 906;
  }
  if ((apexHit.entity != pyramid) || (apexHit.distance != 9.5F) ||
      (apexHit.point.y != 0.5F)) {
    return 907;
  }

  // Outside the triangular footprint but inside the old bounding box: the
  // hull must not report a hit there.
  engine::runtime::PhysicsRaycastHit cornerHit{};
  if (engine::runtime::raycast(*world, engine::math::Vec3(5.45F, 10.0F, 0.4F),
                               engine::math::Vec3(0.0F, -1.0F, 0.0F), 100.0F,
                               &cornerHit)) {
    return 908;
  }

  return 0;
}

// Restitution applies only above the speed threshold: slow pushing contacts
// must fully absorb (no bounce), fast impacts still rebound, so driven
// bodies cannot ratchet themselves airborne against round obstacles.
int check_restitution_speed_threshold() {
  const auto run_case = [](float approachSpeed, float *outVelocity) -> bool {
    std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                      engine::runtime::World());
    if (world == nullptr) {
      return false;
    }
    world->end_frame_phase();

    engine::runtime::Transform movingTransform{};
    const engine::runtime::Entity moving =
        world->create_scene_object(movingTransform);
    engine::runtime::Transform blockerTransform{};
    blockerTransform.position = engine::math::Vec3(0.9F, 0.0F, 0.0F);
    const engine::runtime::Entity blocker =
        world->create_scene_object(blockerTransform);
    if ((moving == engine::runtime::kInvalidEntity) ||
        (blocker == engine::runtime::kInvalidEntity)) {
      return false;
    }

    engine::runtime::Collider sphere{};
    sphere.shape = engine::runtime::ColliderShape::Sphere;
    sphere.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
    sphere.restitution = 0.3F;
    engine::runtime::RigidBody body{};
    body.inverseMass = 1.0F;
    body.velocity = engine::math::Vec3(approachSpeed, 0.0F, 0.0F);
    if (!world->add_collider(moving, sphere) ||
        !world->add_collider(blocker, sphere) ||
        !world->add_rigid_body(moving, body)) {
      return false;
    }

    world->begin_update_phase();
    const bool resolved = engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    if (!resolved) {
      return false;
    }

    const engine::runtime::RigidBody *after =
        world->get_rigid_body_ptr(moving);
    if (after == nullptr) {
      return false;
    }
    *outVelocity = after->velocity.x;
    return true;
  };

  float slowVelocity = -1.0F;
  if (!run_case(0.5F, &slowVelocity) || (slowVelocity != 0.0F)) {
    return 950;
  }

  float fastVelocity = 0.0F;
  if (!run_case(5.0F, &fastVelocity) ||
      (fastVelocity != (5.0F - ((1.0F + 0.3F) * 5.0F)))) {
    return 951;
  }

  return 0;
}

/// H-07 boundary: a collider whose expanded cell footprint exceeds the
/// per-collider cell budget (a compound child 200*sqrt(2) m from its
/// spinning root sees omega x r ~ 3400 m/s of point velocity, a ~484-cell
/// footprint against the 256 cap) must divert to the brute-force overflow
/// list: its overlapping pair is still detected (lossless), exactly one
/// warning episode is recorded while the condition persists across steps,
/// and a second episode is counted only after the condition clears and
/// returns. The wall collider carries a small local rotation because the
/// compound pair routes through GJK, whose support ties on exactly
/// axis-aligned face-face overlaps collapse the simplex (pre-existing
/// narrow-phase limitation, tracked separately from this broad-phase
/// coverage).
int check_broadphase_overflow_lossless_and_loud() {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 960;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto root = world->create_entity();
  engine::runtime::Transform rootT{};
  world->add_transform(root, rootT);
  engine::runtime::RigidBody rootRB{};
  rootRB.inverseMass = 1.0F;
  rootRB.angularVelocity = engine::math::Vec3(0.0F, 0.0F, 12.0F);
  world->add_rigid_body(root, rootRB);

  const auto child = world->create_entity();
  engine::runtime::Transform childT{};
  childT.position = engine::math::Vec3(200.0F, 200.0F, 0.0F);
  childT.parentId = world->persistent_id(root);
  world->add_transform(child, childT);
  engine::runtime::Collider childCol{};
  childCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(child, childCol);

  const auto wall = world->create_entity();
  engine::runtime::Transform wallT{};
  wallT.position = engine::math::Vec3(200.6F, 200.0F, 0.0F);
  world->add_transform(wall, wallT);
  engine::runtime::Collider wallCol{};
  wallCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  wallCol.localRotation = engine::math::from_axis_angle(
      engine::math::normalize(engine::math::Vec3(1.0F, 1.0F, 1.0F)), 0.2F);
  world->add_collider(wall, wallCol);

  auto run_resolve = [&world]() noexcept -> bool {
    world->begin_update_phase();
    const bool resolved = engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
    return resolved;
  };

  if (!run_resolve()) {
    return 961;
  }
  const engine::physics::PhysicsContext &ctx = world->physics_context();
  if (ctx.broadphaseOverflowEpisodes != 1U) {
    return 962;
  }
  if (ctx.collisionPairCount < 1U) {
    return 963;
  }

  {
    engine::runtime::RigidBody *rb = world->get_rigid_body_ptr(root);
    if (rb == nullptr) {
      return 964;
    }
    rb->angularVelocity = engine::math::Vec3(0.0F, 0.0F, 12.0F);
  }
  if (!run_resolve() || (ctx.broadphaseOverflowEpisodes != 1U)) {
    return 965;
  }

  {
    engine::runtime::RigidBody *rb = world->get_rigid_body_ptr(root);
    if (rb == nullptr) {
      return 966;
    }
    rb->angularVelocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  }
  if (!run_resolve() || (ctx.broadphaseOverflowEpisodes != 1U) ||
      ctx.broadphaseOverflowActive) {
    return 967;
  }

  {
    engine::runtime::RigidBody *rb = world->get_rigid_body_ptr(root);
    if (rb == nullptr) {
      return 968;
    }
    rb->angularVelocity = engine::math::Vec3(0.0F, 0.0F, 12.0F);
  }
  if (!run_resolve() || (ctx.broadphaseOverflowEpisodes != 2U)) {
    return 969;
  }

  return 0;
}

/// H-08 regression: exactly coincident spheres have no center-difference
/// direction, so the contact must use the documented deterministic +Y
/// fallback instead of a zero normal that leaves both bodies embedded.
/// After one resolve the full overlap is corrected, so the pair must sit
/// ~sumR apart along Y (0.9 lower bound leaves room for the 1e-4 internal
/// distance epsilon) with X/Z untouched (exact: the +Y normal has zero
/// X/Z components by construction).
int check_coincident_spheres_separate() {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1050;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  engine::runtime::Entity spheres[2] = {};
  for (int i = 0; i < 2; ++i) {
    spheres[i] = world->create_entity();
    engine::runtime::Transform t{};
    t.position = engine::math::Vec3(3.0F, 3.0F, 3.0F);
    world->add_transform(spheres[i], t);
    engine::runtime::Collider col{};
    col.shape = engine::runtime::ColliderShape::Sphere;
    col.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
    world->add_collider(spheres[i], col);
    engine::runtime::RigidBody rb{};
    rb.inverseMass = 1.0F;
    world->add_rigid_body(spheres[i], rb);
  }

  world->begin_update_phase();
  const bool resolved = engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  if (!resolved) {
    return 1051;
  }

  engine::runtime::Transform tA{};
  engine::runtime::Transform tB{};
  if (!world->get_transform(spheres[0], &tA) ||
      !world->get_transform(spheres[1], &tB)) {
    return 1052;
  }
  const float dy = tB.position.y - tA.position.y;
  if ((dy < 0.9F) || (tA.position.x != 3.0F) || (tB.position.x != 3.0F) ||
      (tA.position.z != 3.0F) || (tB.position.z != 3.0F)) {
    return 1053;
  }
  return 0;
}

/// H-08 regression: a sphere whose center lies INSIDE a box must exit
/// through the nearest face with penetration = face distance + radius.
/// Sphere r=0.25 at (0.8, 0.1, 0) inside a unit-half-extent static box:
/// nearest face is +X at 0.2, so the sphere must land at x = 1.25
/// (touching the face) with Y/Z unchanged. Tolerance 1e-4 covers the
/// clamped-closest-point float arithmetic; the old fallback pushed +Y by
/// only the radius, leaving the sphere embedded.
int check_sphere_inside_box_exits_nearest_face() {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1060;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto box = world->create_entity();
  engine::runtime::Transform boxT{};
  world->add_transform(box, boxT);
  engine::runtime::Collider boxCol{};
  boxCol.shape = engine::runtime::ColliderShape::AABB;
  boxCol.halfExtents = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  world->add_collider(box, boxCol);

  const auto sphere = world->create_entity();
  engine::runtime::Transform sphT{};
  sphT.position = engine::math::Vec3(0.8F, 0.1F, 0.0F);
  world->add_transform(sphere, sphT);
  engine::runtime::Collider sphCol{};
  sphCol.shape = engine::runtime::ColliderShape::Sphere;
  sphCol.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);
  world->add_collider(sphere, sphCol);
  engine::runtime::RigidBody rb{};
  rb.inverseMass = 1.0F;
  world->add_rigid_body(sphere, rb);

  world->begin_update_phase();
  const bool resolved = engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  if (!resolved) {
    return 1061;
  }

  engine::runtime::Transform after{};
  if (!world->get_transform(sphere, &after)) {
    return 1062;
  }
  if ((std::fabs(after.position.x - 1.25F) > 1.0e-4F) ||
      (after.position.y != 0.1F) || (after.position.z != 0.0F)) {
    return 1063;
  }
  return 0;
}

/// H-08 regression: when both bodies are dynamic the contact impulse must
/// use the full normal-row Jacobian (point relative velocity and the
/// i |r x n|^2 effective-mass terms). Box A (invMass 1, invInertia 1) at
/// origin, sphere B approaching a tangentially offset face point at
/// 0.5 m/s (below the restitution threshold, so the target normal speed
/// is exactly zero). After one resolve the recomputed point-relative
/// normal velocity must sit within 5e-3 of zero (float roundings across
/// the impulse chain); the old linear-only effective mass with an angular
/// response applied overshot by ~2e-2.
int check_angular_effective_mass_no_overshoot() {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1070;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto boxEntity = world->create_entity();
  engine::runtime::Transform boxT{};
  world->add_transform(boxEntity, boxT);
  engine::runtime::Collider boxCol{};
  boxCol.shape = engine::runtime::ColliderShape::AABB;
  boxCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(boxEntity, boxCol);
  engine::runtime::RigidBody boxRB{};
  boxRB.inverseMass = 1.0F;
  boxRB.inverseInertia = 1.0F;
  world->add_rigid_body(boxEntity, boxRB);

  const auto sphereEntity = world->create_entity();
  engine::runtime::Transform sphT{};
  sphT.position = engine::math::Vec3(0.7F, 0.3F, 0.0F);
  world->add_transform(sphereEntity, sphT);
  engine::runtime::Collider sphCol{};
  sphCol.shape = engine::runtime::ColliderShape::Sphere;
  sphCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  world->add_collider(sphereEntity, sphCol);
  engine::runtime::RigidBody sphRB{};
  sphRB.inverseMass = 1.0F;
  sphRB.inverseInertia = 1.0F;
  sphRB.velocity = engine::math::Vec3(-0.5F, 0.0F, 0.0F);
  world->add_rigid_body(sphereEntity, sphRB);

  world->begin_update_phase();
  const bool resolved = engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  if (!resolved) {
    return 1071;
  }

  engine::runtime::Transform boxAfter{};
  engine::runtime::Transform sphAfter{};
  const engine::runtime::RigidBody *bodyA =
      world->get_rigid_body_ptr(boxEntity);
  const engine::runtime::RigidBody *bodyB =
      world->get_rigid_body_ptr(sphereEntity);
  if (!world->get_transform(boxEntity, &boxAfter) ||
      !world->get_transform(sphereEntity, &sphAfter) || (bodyA == nullptr) ||
      (bodyB == nullptr)) {
    return 1072;
  }

  const engine::math::Vec3 contact(0.5F, 0.3F, 0.0F);
  const engine::math::Vec3 normal(1.0F, 0.0F, 0.0F);
  const engine::math::Vec3 rA =
      engine::math::sub(contact, boxAfter.position);
  const engine::math::Vec3 rB =
      engine::math::sub(contact, sphAfter.position);
  const engine::math::Vec3 pointVelA = engine::math::add(
      bodyA->velocity, engine::math::cross(bodyA->angularVelocity, rA));
  const engine::math::Vec3 pointVelB = engine::math::add(
      bodyB->velocity, engine::math::cross(bodyB->angularVelocity, rB));
  const float residual = engine::math::dot(
      engine::math::sub(pointVelB, pointVelA), normal);
  if (std::fabs(residual) > 5.0e-3F) {
    return 1073;
  }
  return 0;
}

/// H-08 regression: a convex hull against a heightfield must consume the
/// hull's real support function, not its declared-halfExtents box. A
/// 16-slice cylinder hull (r=0.5, hh=0.5) floats above a 45-degree
/// diagonal slope (plane y = x + z, normal (-1,1,-1)/sqrt(3)) at plane
/// distance 0.78: the box model reaches 0.866 along the normal (false
/// contact) while the true cylinder support reaches at most ~0.70, so a
/// correct narrow phase reports nothing and the body must stay bitwise
/// unmoved with zero recorded pairs.
int check_heightfield_hull_uses_real_shape() {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1080;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const auto terrain = world->create_entity();
  engine::runtime::Transform terrainT{};
  world->add_transform(terrain, terrainT);
  engine::runtime::Collider terrainCol{};
  terrainCol.shape = engine::runtime::ColliderShape::Heightfield;
  terrainCol.halfExtents = engine::math::Vec3(2.0F, 3.0F, 2.0F);
  world->add_collider(terrain, terrainCol);
  auto heightfield = std::unique_ptr<engine::physics::HeightfieldData>(
      new (std::nothrow) engine::physics::HeightfieldData());
  if (heightfield == nullptr) {
    return 1081;
  }
  heightfield->rows = 2U;
  heightfield->columns = 2U;
  heightfield->spacingX = 2.0F;
  heightfield->spacingZ = 2.0F;
  heightfield->heights[0] = -2.0F;
  heightfield->heights[1] = 0.0F;
  heightfield->heights[2] = 0.0F;
  heightfield->heights[3] = 2.0F;
  if (!engine::physics::set_heightfield_data(world->physics_context(), terrain,
                                             *heightfield)) {
    return 1082;
  }

  const auto hullEntity = world->create_entity();
  engine::runtime::Transform hullT{};
  hullT.position = engine::math::Vec3(0.0F, 1.352F, 0.0F);
  world->add_transform(hullEntity, hullT);
  engine::runtime::Collider hullCol{};
  hullCol.shape = engine::runtime::ColliderShape::ConvexHull;
  hullCol.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  hullCol.hullSource = engine::runtime::HullSource::Cylinder;
  world->add_collider(hullEntity, hullCol);
  engine::runtime::RigidBody hullRB{};
  hullRB.inverseMass = 1.0F;
  world->add_rigid_body(hullEntity, hullRB);

  world->begin_update_phase();
  const bool resolved = engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  if (!resolved) {
    return 1083;
  }

  engine::runtime::Transform after{};
  if (!world->get_transform(hullEntity, &after)) {
    return 1084;
  }
  if ((after.position.x != 0.0F) || (after.position.y != 1.352F) ||
      (after.position.z != 0.0F) ||
      (world->physics_context().collisionPairCount != 0U)) {
    return 1085;
  }
  return 0;
}

/// H-08 boundary: contacts past kMaxCollisionPairs must be counted and
/// reported once per overflow episode instead of vanishing silently, the
/// kept set must fill the buffer exactly, and a workload trimmed back to
/// the cap must clear the episode without logging a new one. 1030
/// well-separated static overlapping pairs produce 6 drops; removing six
/// pairs' colliders lands exactly at capacity (the zero-drop boundary).
int check_collision_pair_cap_loud() {
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1090;
  }
  world->end_frame_phase();
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  constexpr int kPairCount = 1030;
  engine::runtime::Entity extras[12] = {};
  int extraCount = 0;
  for (int p = 0; p < kPairCount; ++p) {
    for (int half = 0; half < 2; ++half) {
      const auto entity = world->create_entity();
      engine::runtime::Transform t{};
      t.position = engine::math::Vec3(static_cast<float>(p) * 10.0F +
                                          (static_cast<float>(half) * 0.5F),
                                      0.0F, 0.0F);
      world->add_transform(entity, t);
      engine::runtime::Collider col{};
      col.shape = engine::runtime::ColliderShape::Sphere;
      col.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
      world->add_collider(entity, col);
      if ((p >= kPairCount - 6) && (extraCount < 12)) {
        extras[extraCount] = entity;
        ++extraCount;
      }
    }
  }

  auto run_resolve = [&world]() noexcept -> bool {
    world->begin_update_phase();
    const bool resolved = engine::runtime::resolve_collisions(*world);
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
    return resolved;
  };

  const engine::physics::PhysicsContext &ctx = world->physics_context();
  if (!run_resolve() ||
      (ctx.collisionPairCount != engine::physics::kMaxCollisionPairs) ||
      (ctx.collisionPairDropCount != 6U) ||
      (ctx.collisionPairOverflowEpisodes != 1U)) {
    return 1091;
  }
  if (!run_resolve() || (ctx.collisionPairOverflowEpisodes != 1U)) {
    return 1092;
  }

  for (int i = 0; i < extraCount; ++i) {
    if (!world->remove_collider(extras[i])) {
      return 1093;
    }
  }
  if (!run_resolve() ||
      (ctx.collisionPairCount != engine::physics::kMaxCollisionPairs) ||
      (ctx.collisionPairDropCount != 0U) ||
      ctx.collisionPairOverflowActive ||
      (ctx.collisionPairOverflowEpisodes != 1U)) {
    return 1094;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_physics_cvars_register_after_core_cvars();
  if (result != 0) {
    return result;
  }

  result = check_primitive_hull_collision();
  if (result != 0) {
    return result;
  }

  result = check_restitution_speed_threshold();
  if (result != 0) {
    return result;
  }

  result = check_gravity_step();
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

  result = check_resolve_collisions_uses_delta_seconds();
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

  result = check_broadphase_overflow_lossless_and_loud();
  if (result != 0) {
    return result;
  }

  result = check_coincident_spheres_separate();
  if (result != 0) {
    return result;
  }

  result = check_sphere_inside_box_exits_nearest_face();
  if (result != 0) {
    return result;
  }

  result = check_angular_effective_mass_no_overshoot();
  if (result != 0) {
    return result;
  }

  result = check_heightfield_hull_uses_real_shape();
  if (result != 0) {
    return result;
  }

  result = check_collision_pair_cap_loud();
  if (result != 0) {
    return result;
  }

  result = check_capsule_vs_aabb_collision();
  if (result != 0) {
    return result;
  }

  result = check_capsule_vs_sphere_collision();
  if (result != 0) {
    return result;
  }

  result = check_capsule_vs_capsule_collision();
  if (result != 0) {
    return result;
  }

  result = check_raycast_hits_capsule();
  if (result != 0) {
    return result;
  }

  result = check_convex_hull_build();
  if (result != 0) {
    return result;
  }

  result = check_convex_hull_vs_aabb_collision();
  if (result != 0) {
    return result;
  }

  result = check_raycast_hits_convex_hull();
  if (result != 0) {
    return result;
  }

  result = check_heightfield_vs_sphere_collision();
  if (result != 0) {
    return result;
  }

  result = check_heightfield_object_outside_grid_negative();
  if (result != 0) {
    return result;
  }

  result = check_raycast_hits_heightfield();
  if (result != 0) {
    return result;
  }

  result = check_shape_payload_world_isolation();
  if (result != 0) {
    return result;
  }

  result = check_shape_payloads_do_not_survive_entity_reuse();
  if (result != 0) {
    return result;
  }

  result = check_collider_replacement_prunes_payloads();
  if (result != 0) {
    return result;
  }

  result = check_invalid_shape_payloads_rejected();
  if (result != 0) {
    return result;
  }

  return 0;
}
