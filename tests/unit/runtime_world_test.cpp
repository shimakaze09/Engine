// Verifies runtime world test behavior for the Engine test suite.

#include <cmath>
#include <memory>
#include <new>

#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

int verify_persistent_id_index() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  const engine::runtime::Entity first =
      world->create_entity_with_persistent_id(1001U);
  if (first == engine::runtime::kInvalidEntity) {
    return 2;
  }

  if (world->create_entity_with_persistent_id(1001U) !=
      engine::runtime::kInvalidEntity) {
    return 3;
  }

  if (world->find_entity_by_persistent_id(1001U) != first) {
    return 4;
  }

  if (!world->destroy_entity(first)) {
    return 5;
  }

  if (world->find_entity_by_persistent_id(1001U) !=
      engine::runtime::kInvalidEntity) {
    return 6;
  }

  const engine::runtime::Entity recreated =
      world->create_entity_with_persistent_id(1001U);
  if (recreated == engine::runtime::kInvalidEntity) {
    return 7;
  }

  if (world->persistent_id(recreated) != 1001U) {
    return 8;
  }

  return 0;
}

int verify_hierarchical_transform_propagation() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 20;
  }

  const engine::runtime::Entity parent = world->create_entity();
  const engine::runtime::Entity child = world->create_entity();
  const engine::runtime::Entity sibling = world->create_entity();
  if ((parent == engine::runtime::kInvalidEntity) ||
      (child == engine::runtime::kInvalidEntity) ||
      (sibling == engine::runtime::kInvalidEntity)) {
    return 21;
  }

  engine::runtime::Transform parentTransform{};
  parentTransform.position = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  if (!world->add_transform(parent, parentTransform)) {
    return 22;
  }

  engine::runtime::Transform childTransform{};
  childTransform.position = engine::math::Vec3(0.0F, 2.0F, 0.0F);
  childTransform.parentId = world->persistent_id(parent);
  if (!world->add_transform(child, childTransform)) {
    return 23;
  }

  engine::runtime::Transform siblingTransform{};
  siblingTransform.position = engine::math::Vec3(10.0F, 1.0F, 0.0F);
  if (!world->add_transform(sibling, siblingTransform)) {
    return 24;
  }

  world->begin_render_prep_phase();

  const engine::runtime::WorldTransform *childWorld =
      world->get_world_transform_read_ptr(child);
  const engine::runtime::WorldTransform *siblingWorld =
      world->get_world_transform_read_ptr(sibling);
  if ((childWorld == nullptr) || (siblingWorld == nullptr)) {
    return 25;
  }

  if (!nearly_equal(childWorld->position.x, 1.0F) ||
      !nearly_equal(childWorld->position.y, 2.0F) ||
      !nearly_equal(siblingWorld->position.x, 10.0F)) {
    return 26;
  }

  world->begin_render_phase();
  world->end_frame_phase();

  world->begin_update_phase();
  const auto simTokenA = world->simulation_access_token();
  engine::runtime::Transform *parentWrite =
      world->get_transform_write_ptr(parent, simTokenA);
  if (parentWrite == nullptr) {
    return 27;
  }
  parentWrite->position.x = 3.0F;
  world->commit_update_phase();

  world->begin_render_prep_phase();
  childWorld = world->get_world_transform_read_ptr(child);
  siblingWorld = world->get_world_transform_read_ptr(sibling);
  if ((childWorld == nullptr) || (siblingWorld == nullptr)) {
    return 28;
  }

  if (!nearly_equal(childWorld->position.x, 3.0F) ||
      !nearly_equal(childWorld->position.y, 2.0F) ||
      !nearly_equal(siblingWorld->position.x, 10.0F)) {
    return 29;
  }

  world->begin_render_phase();
  world->end_frame_phase();

  world->begin_update_phase();
  const auto simTokenB = world->simulation_access_token();
  engine::runtime::Transform *childWrite =
      world->get_transform_write_ptr(child, simTokenB);
  if (childWrite == nullptr) {
    return 30;
  }
  childWrite->parentId = engine::runtime::kInvalidPersistentId;
  childWrite->position.x = 5.0F;
  world->commit_update_phase();

  world->begin_render_prep_phase();
  childWorld = world->get_world_transform_read_ptr(child);
  if (childWorld == nullptr) {
    return 31;
  }

  if (!nearly_equal(childWorld->position.x, 5.0F) ||
      !nearly_equal(childWorld->position.y, 2.0F)) {
    return 32;
  }

  world->begin_render_phase();
  world->end_frame_phase();
  return 0;
}

int verify_transform_cycle_is_stable() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 40;
  }

  const engine::runtime::Entity first = world->create_entity();
  const engine::runtime::Entity second = world->create_entity();
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity)) {
    return 41;
  }

  engine::runtime::Transform firstTransform{};
  firstTransform.position = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  firstTransform.parentId = world->persistent_id(second);
  if (!world->add_transform(first, firstTransform)) {
    return 42;
  }

  engine::runtime::Transform secondTransform{};
  secondTransform.position = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  secondTransform.parentId = world->persistent_id(first);
  if (!world->add_transform(second, secondTransform)) {
    return 43;
  }

  world->begin_render_prep_phase();

  if (world->get_world_transform_read_ptr(first) == nullptr) {
    return 44;
  }

  if (world->get_world_transform_read_ptr(second) == nullptr) {
    return 45;
  }

  if (world->world_transform_count() != 2U) {
    return 46;
  }

  world->begin_render_phase();
  world->end_frame_phase();
  return 0;
}

// Parented colliders must collide and answer queries at their composed world
// pose, and parented rigid bodies must never integrate (they are kinematic
// attachments that follow their parent).
int verify_physics_follows_transform_hierarchy() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 90;
  }

  const engine::runtime::Entity parent = world->create_entity();
  const engine::runtime::Entity child = world->create_entity();
  const engine::runtime::Entity root = world->create_entity();
  if ((parent == engine::runtime::kInvalidEntity) ||
      (child == engine::runtime::kInvalidEntity) ||
      (root == engine::runtime::kInvalidEntity)) {
    return 91;
  }

  engine::runtime::Transform parentTransform{};
  parentTransform.position = engine::math::Vec3(10.0F, 0.0F, 0.0F);
  if (!world->add_transform(parent, parentTransform)) {
    return 92;
  }

  engine::runtime::Transform childTransform{};
  childTransform.position = engine::math::Vec3(0.0F, 2.0F, 0.0F);
  childTransform.parentId = world->persistent_id(parent);
  if (!world->add_transform(child, childTransform) ||
      !world->add_collider(child, engine::runtime::Collider{}) ||
      !world->add_rigid_body(child, engine::runtime::RigidBody{})) {
    return 93;
  }

  engine::runtime::Transform rootTransform{};
  rootTransform.position = engine::math::Vec3(20.0F, 0.0F, 0.0F);
  engine::runtime::RigidBody rootBody{};
  rootBody.velocity = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  if (!world->add_transform(root, rootTransform) ||
      !world->add_collider(root, engine::runtime::Collider{}) ||
      !world->add_rigid_body(root, rootBody)) {
    return 94;
  }

  // One frame cycle so the world-transform cache holds the composed poses.
  world->begin_render_prep_phase();
  world->begin_render_phase();
  world->end_frame_phase();

  // Collision transform: composed world pose with the parent link cleared.
  engine::runtime::Transform collisionPose{};
  if (!world->get_collision_transform(child, &collisionPose)) {
    return 95;
  }
  if ((collisionPose.position.x != 10.0F) ||
      (collisionPose.position.y != 2.0F) ||
      (collisionPose.position.z != 0.0F) ||
      (collisionPose.parentId != engine::runtime::kInvalidPersistentId)) {
    return 96;
  }

  // The local transform read stays untouched local space.
  engine::runtime::Transform localPose{};
  if (!world->get_transform(child, &localPose) ||
      (localPose.position.x != 0.0F) || (localPose.position.y != 2.0F)) {
    return 97;
  }

  // A ray down through the child's WORLD position hits it exactly on the
  // top face of its default half-extent-0.5 AABB.
  engine::runtime::PhysicsRaycastHit hit{};
  if (!engine::runtime::raycast(*world, engine::math::Vec3(10.0F, 10.0F, 0.0F),
                                engine::math::Vec3(0.0F, -1.0F, 0.0F), 100.0F,
                                &hit)) {
    return 98;
  }
  if ((hit.entity != child) || (hit.distance != 7.5F) ||
      (hit.point.y != 2.5F)) {
    return 99;
  }

  // A ray down through the child's LOCAL position must miss: the collider
  // lives at the world pose, not the raw local offset.
  engine::runtime::PhysicsRaycastHit localHit{};
  if (engine::runtime::raycast(*world, engine::math::Vec3(0.0F, 10.0F, 0.0F),
                               engine::math::Vec3(0.0F, -1.0F, 0.0F), 100.0F,
                               &localHit)) {
    return 100;
  }

  // Step physics: the parented body must not integrate at all, while the
  // root control body advances by exactly velocity.x * dt.
  const float dt = 1.0F / 60.0F;
  world->begin_update_phase();
  if (!engine::runtime::step_physics(*world, dt)) {
    world->commit_update_phase();
    return 101;
  }
  world->commit_update_phase();

  engine::runtime::Transform childAfter{};
  if (!world->get_transform(child, &childAfter) ||
      (childAfter.position.x != 0.0F) || (childAfter.position.y != 2.0F) ||
      (childAfter.position.z != 0.0F)) {
    return 102;
  }

  engine::runtime::Transform rootAfter{};
  if (!world->get_transform(root, &rootAfter) ||
      (rootAfter.position.x != 20.0F + (1.0F * dt))) {
    return 103;
  }

  return 0;
}

int verify_persistent_index_tombstones() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 80;
  }

  constexpr std::uint32_t kBaseId = 10000U;
  constexpr std::size_t kEntityCount = 256U;
  engine::runtime::Entity entities[kEntityCount]{};

  for (std::size_t i = 0U; i < kEntityCount; ++i) {
    const engine::runtime::PersistentId id =
        kBaseId + static_cast<std::uint32_t>(i * 17U);
    entities[i] = world->create_entity_with_persistent_id(id);
    if (entities[i] == engine::runtime::kInvalidEntity) {
      return 81;
    }
  }

  for (std::size_t i = 0U; i < kEntityCount; i += 2U) {
    if (!world->destroy_entity(entities[i])) {
      return 82;
    }
  }

  for (std::size_t i = 1U; i < kEntityCount; i += 2U) {
    const engine::runtime::PersistentId id =
        kBaseId + static_cast<std::uint32_t>(i * 17U);
    if (world->find_entity_by_persistent_id(id) != entities[i]) {
      return 83;
    }
  }

  for (std::size_t i = 0U; i < kEntityCount; i += 2U) {
    const engine::runtime::PersistentId id =
        kBaseId + static_cast<std::uint32_t>(i * 17U);
    if (world->find_entity_by_persistent_id(id) !=
        engine::runtime::kInvalidEntity) {
      return 84;
    }
  }

  for (std::size_t i = 0U; i < kEntityCount; i += 2U) {
    const engine::runtime::PersistentId replacementId =
        kBaseId + 500000U + static_cast<std::uint32_t>(i * 31U);
    const engine::runtime::Entity entity =
        world->create_entity_with_persistent_id(replacementId);
    if (entity == engine::runtime::kInvalidEntity) {
      return 85;
    }

    if (world->find_entity_by_persistent_id(replacementId) != entity) {
      return 86;
    }
  }

  return 0;
}

int verify_variadic_for_each() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 100;
  }

  // Create entities with different component combinations.
  const engine::runtime::Entity e1 = world->create_entity();
  const engine::runtime::Entity e2 = world->create_entity();
  const engine::runtime::Entity e3 = world->create_entity();
  if ((e1 == engine::runtime::kInvalidEntity) ||
      (e2 == engine::runtime::kInvalidEntity) ||
      (e3 == engine::runtime::kInvalidEntity)) {
    return 101;
  }

  // e1: Transform + RigidBody + Collider
  engine::runtime::Transform t1{};
  t1.position = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  if (!world->add_transform(e1, t1)) {
    return 102;
  }

  engine::runtime::RigidBody rb1{};
  rb1.inverseMass = 1.0F;
  if (!world->add_rigid_body(e1, rb1)) {
    return 103;
  }

  engine::runtime::Collider col1{};
  col1.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_collider(e1, col1)) {
    return 104;
  }

  // e2: Transform + RigidBody (no Collider)
  engine::runtime::Transform t2{};
  t2.position = engine::math::Vec3(2.0F, 0.0F, 0.0F);
  if (!world->add_transform(e2, t2)) {
    return 105;
  }

  engine::runtime::RigidBody rb2{};
  rb2.inverseMass = 0.5F;
  if (!world->add_rigid_body(e2, rb2)) {
    return 106;
  }

  // e3: Transform only
  engine::runtime::Transform t3{};
  t3.position = engine::math::Vec3(3.0F, 0.0F, 0.0F);
  if (!world->add_transform(e3, t3)) {
    return 107;
  }

  // 3-component query: only e1 has Transform + RigidBody + Collider.
  int tripleCount = 0;
  engine::runtime::Entity tripleEntity{};
  world->for_each<engine::runtime::Transform, engine::runtime::RigidBody,
                  engine::runtime::Collider>(
      [&](engine::runtime::Entity entity, const engine::runtime::Transform &,
          const engine::runtime::RigidBody &,
          const engine::runtime::Collider &) noexcept {
        ++tripleCount;
        tripleEntity = entity;
      });

  if ((tripleCount != 1) || (tripleEntity.index != e1.index)) {
    return 108;
  }

  // 2-component query: e1 and e2 have Transform + RigidBody.
  int pairCount = 0;
  world->for_each<engine::runtime::Transform, engine::runtime::RigidBody>(
      [&](engine::runtime::Entity, const engine::runtime::Transform &,
          const engine::runtime::RigidBody &) noexcept { ++pairCount; });

  if (pairCount != 2) {
    return 109;
  }

  // Single-component query: all 3 have Transform.
  int singleCount = 0;
  world->for_each<engine::runtime::Transform>(
      [&](engine::runtime::Entity,
          const engine::runtime::Transform &) noexcept { ++singleCount; });

  if (singleCount != 3) {
    return 110;
  }

  return 0;
}

/// Verifies begin_play_pending_count tracks create/mark/destroy exactly.
int verify_begin_play_pending_count() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 120;
  }

  if (world->begin_play_pending_count() != 0U) {
    return 121;
  }

  const engine::runtime::Entity first = world->create_entity();
  const engine::runtime::Entity second = world->create_entity();
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity)) {
    return 122;
  }
  if (world->begin_play_pending_count() != 2U) {
    return 123;
  }

  world->mark_begin_play_done(first);
  if (world->begin_play_pending_count() != 1U) {
    return 124;
  }

  // Marking twice must not double-decrement.
  world->mark_begin_play_done(first);
  if (world->begin_play_pending_count() != 1U) {
    return 125;
  }

  // Destroying an entity that never fired begin_play clears its pending slot.
  if (!world->destroy_entity(second)) {
    return 126;
  }
  if (world->begin_play_pending_count() != 0U) {
    return 127;
  }

  // A recycled index starts pending again.
  const engine::runtime::Entity third = world->create_entity();
  if (third == engine::runtime::kInvalidEntity) {
    return 128;
  }
  if (world->begin_play_pending_count() != 1U) {
    return 129;
  }

  return 0;
}

/// Verifies destruction removes scripts from dense component iteration.
int verify_destroy_removes_script_component() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 140;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 141;
  }

  engine::runtime::ScriptComponent script{};
  script.scriptPath[0] = 'x';
  script.scriptPath[1] = '\0';
  if (!world->add_script_component(entity, script)) {
    return 142;
  }

  std::size_t scriptCount = 0U;
  world->for_each<engine::runtime::ScriptComponent>(
      [&scriptCount](engine::runtime::Entity,
                     const engine::runtime::ScriptComponent &) noexcept {
        ++scriptCount;
      });
  if (scriptCount != 1U) {
    return 143;
  }

  if (!world->destroy_entity(entity)) {
    return 144;
  }

  scriptCount = 0U;
  world->for_each<engine::runtime::ScriptComponent>(
      [&scriptCount](engine::runtime::Entity,
                     const engine::runtime::ScriptComponent &) noexcept {
        ++scriptCount;
      });
  if (scriptCount != 0U) {
    return 145;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = verify_persistent_id_index();
  if (result != 0) {
    return result;
  }

  result = verify_begin_play_pending_count();
  if (result != 0) {
    return result;
  }

  result = verify_destroy_removes_script_component();
  if (result != 0) {
    return result;
  }

  result = verify_hierarchical_transform_propagation();
  if (result != 0) {
    return result;
  }

  result = verify_persistent_index_tombstones();
  if (result != 0) {
    return result;
  }

  result = verify_transform_cycle_is_stable();
  if (result != 0) {
    return result;
  }

  result = verify_physics_follows_transform_hierarchy();
  if (result != 0) {
    return result;
  }

  return verify_variadic_for_each();
}
