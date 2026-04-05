#include <cmath>
#include <memory>
#include <new>

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

  return 0;
}
