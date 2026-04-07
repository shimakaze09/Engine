#include <cmath>
#include <memory>
#include <new>

#include "engine/math/quat.h"
#include "engine/physics/physics.h"
#include "engine/runtime/world.h"

namespace {

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
  if (!engine::physics::step_physics(*world, 1.0F / 60.0F)) {
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

  if (!engine::physics::resolve_collisions(*world)) {
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

  if (!engine::physics::resolve_collisions(*world)) {
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
  if (!engine::physics::step_physics(*world, 1.0F / 60.0F)) {
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
  if (!engine::physics::resolve_collisions(*world)) {
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
  if (!engine::physics::step_physics(*world, 1.0F / 60.0F)) {
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
  if (!engine::physics::resolve_collisions(*world)) {
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
  if (!engine::physics::resolve_collisions(*world)) {
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
  if (!engine::physics::resolve_collisions(*world)) {
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

  return 0;
}
