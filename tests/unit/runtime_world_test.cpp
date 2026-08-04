// Verifies runtime world test behavior for the Engine test suite.

#include <cmath>
#include <limits>
#include <memory>
#include <new>

#include "engine/physics/physics.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

int verify_raw_and_scene_object_creation() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 150;
  }

  const engine::runtime::Entity raw = world->create_entity();
  if (raw == engine::runtime::kInvalidEntity) {
    return 151;
  }
  if ((world->get_transform_read_ptr(raw) != nullptr) ||
      (world->get_world_transform_read_ptr(raw) != nullptr)) {
    return 152;
  }

  const engine::runtime::Entity sceneObject = world->create_scene_object();
  if (sceneObject == engine::runtime::kInvalidEntity) {
    return 153;
  }

  engine::runtime::Transform identityLocal{};
  const engine::runtime::WorldTransform *identityWorld =
      world->get_world_transform_read_ptr(sceneObject);
  if (!world->get_transform(sceneObject, &identityLocal) ||
      (identityWorld == nullptr)) {
    return 154;
  }
  if ((identityLocal.position.x != 0.0F) ||
      (identityLocal.position.y != 0.0F) ||
      (identityLocal.position.z != 0.0F) ||
      (identityLocal.rotation.x != 0.0F) ||
      (identityLocal.rotation.y != 0.0F) ||
      (identityLocal.rotation.z != 0.0F) ||
      (identityLocal.rotation.w != 1.0F) || (identityLocal.scale.x != 1.0F) ||
      (identityLocal.scale.y != 1.0F) || (identityLocal.scale.z != 1.0F) ||
      (identityLocal.parentId != engine::runtime::kInvalidPersistentId)) {
    return 155;
  }
  if ((identityWorld->position.x != 0.0F) ||
      (identityWorld->position.y != 0.0F) ||
      (identityWorld->position.z != 0.0F) ||
      (identityWorld->rotation.x != 0.0F) ||
      (identityWorld->rotation.y != 0.0F) ||
      (identityWorld->rotation.z != 0.0F) ||
      (identityWorld->rotation.w != 1.0F) || (identityWorld->scale.x != 1.0F) ||
      (identityWorld->scale.y != 1.0F) || (identityWorld->scale.z != 1.0F)) {
    return 156;
  }

  engine::runtime::Transform configuredLocal{};
  configuredLocal.position = engine::math::Vec3(2.0F, -3.0F, 4.0F);
  configuredLocal.rotation = engine::math::Quat(0.0F, 0.0F, 1.0F, 0.0F);
  configuredLocal.scale = engine::math::Vec3(0.5F, 2.0F, 4.0F);
  constexpr engine::runtime::PersistentId kConfiguredId = 9001U;
  const engine::runtime::Entity configured =
      world->create_scene_object_with_persistent_id(kConfiguredId,
                                                    configuredLocal);
  if ((configured == engine::runtime::kInvalidEntity) ||
      (world->persistent_id(configured) != kConfiguredId) ||
      (world->find_entity_by_persistent_id(kConfiguredId) != configured)) {
    return 157;
  }

  engine::runtime::Transform configuredRead{};
  if (!world->get_transform(configured, &configuredRead)) {
    return 158;
  }
  if ((configuredRead.position.x != 2.0F) ||
      (configuredRead.position.y != -3.0F) ||
      (configuredRead.position.z != 4.0F) ||
      (configuredRead.rotation.x != 0.0F) ||
      (configuredRead.rotation.y != 0.0F) ||
      (configuredRead.rotation.z != 1.0F) ||
      (configuredRead.rotation.w != 0.0F) || (configuredRead.scale.x != 0.5F) ||
      (configuredRead.scale.y != 2.0F) || (configuredRead.scale.z != 4.0F)) {
    return 159;
  }

  if ((world->alive_entity_count() != 3U) || (world->transform_count() != 2U) ||
      (world->world_transform_count() != 2U)) {
    return 160;
  }
  return 0;
}

int verify_parent_trs_propagation_exact() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 170;
  }

  engine::runtime::Transform parentLocal{};
  parentLocal.position = engine::math::Vec3(10.0F, 20.0F, 30.0F);
  parentLocal.rotation = engine::math::Quat(0.0F, 0.0F, 1.0F, 0.0F);
  parentLocal.scale = engine::math::Vec3(2.0F, 3.0F, 4.0F);
  const engine::runtime::Entity parent =
      world->create_scene_object(parentLocal);
  if (parent == engine::runtime::kInvalidEntity) {
    return 171;
  }

  engine::runtime::Transform childLocal{};
  childLocal.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  childLocal.rotation = engine::math::Quat(1.0F, 0.0F, 0.0F, 0.0F);
  childLocal.scale = engine::math::Vec3(0.5F, 2.0F, 0.25F);
  childLocal.parentId = world->persistent_id(parent);
  const engine::runtime::Entity child = world->create_scene_object(childLocal);
  if (child == engine::runtime::kInvalidEntity) {
    return 172;
  }

  world->begin_render_prep_phase();
  const engine::runtime::WorldTransform *childWorld =
      world->get_world_transform_read_ptr(child);
  if (childWorld == nullptr) {
    return 173;
  }

  if ((childWorld->position.x != 8.0F) || (childWorld->position.y != 14.0F) ||
      (childWorld->position.z != 42.0F) || (childWorld->rotation.x != 0.0F) ||
      (childWorld->rotation.y != 1.0F) || (childWorld->rotation.z != 0.0F) ||
      (childWorld->rotation.w != 0.0F) || (childWorld->scale.x != 1.0F) ||
      (childWorld->scale.y != 6.0F) || (childWorld->scale.z != 1.0F)) {
    return 174;
  }

  const engine::math::Mat4 &matrix = childWorld->matrix;
  if ((matrix.columns[0].x != -1.0F) || (matrix.columns[0].y != 0.0F) ||
      (matrix.columns[0].z != 0.0F) || (matrix.columns[0].w != 0.0F) ||
      (matrix.columns[1].x != 0.0F) || (matrix.columns[1].y != 6.0F) ||
      (matrix.columns[1].z != 0.0F) || (matrix.columns[1].w != 0.0F) ||
      (matrix.columns[2].x != 0.0F) || (matrix.columns[2].y != 0.0F) ||
      (matrix.columns[2].z != -1.0F) || (matrix.columns[2].w != 0.0F) ||
      (matrix.columns[3].x != 8.0F) || (matrix.columns[3].y != 14.0F) ||
      (matrix.columns[3].z != 42.0F) || (matrix.columns[3].w != 1.0F)) {
    return 175;
  }

  world->begin_render_phase();
  world->end_frame_phase();
  return 0;
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
// pose, and a body-less attached collider must never integrate (it follows
// its parent through transform propagation).
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
      !world->add_collider(child, engine::runtime::Collider{})) {
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

  // Physics transform: the hierarchy-composed world pose.
  engine::physics::PhysicsTransform collisionPose{};
  if (!world->get_physics_transform(child, &collisionPose)) {
    return 95;
  }
  if ((collisionPose.position.x != 10.0F) ||
      (collisionPose.position.y != 2.0F) ||
      (collisionPose.position.z != 0.0F)) {
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

  // Step physics: the body-less attached collider must not integrate, while
  // the root control body advances by exactly velocity.x * dt.
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

// Destroying a parent must take its whole transform subtree with it; an
// unrelated sibling hierarchy survives untouched.
int verify_cascade_destroy_subtree() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 110;
  }

  const engine::runtime::Entity parent = world->create_scene_object();
  engine::runtime::Transform childTransform{};
  childTransform.parentId = world->persistent_id(parent);
  const engine::runtime::Entity child =
      world->create_scene_object(childTransform);
  engine::runtime::Transform grandchildTransform{};
  grandchildTransform.parentId = world->persistent_id(child);
  const engine::runtime::Entity grandchild =
      world->create_scene_object(grandchildTransform);
  const engine::runtime::Entity bystander = world->create_scene_object();
  engine::runtime::Transform bystanderChildTransform{};
  bystanderChildTransform.parentId = world->persistent_id(bystander);
  const engine::runtime::Entity bystanderChild =
      world->create_scene_object(bystanderChildTransform);

  if ((parent == engine::runtime::kInvalidEntity) ||
      (child == engine::runtime::kInvalidEntity) ||
      (grandchild == engine::runtime::kInvalidEntity) ||
      (bystander == engine::runtime::kInvalidEntity) ||
      (bystanderChild == engine::runtime::kInvalidEntity)) {
    return 111;
  }

  if (!world->destroy_entity(parent)) {
    return 112;
  }

  if (world->is_alive(parent) || world->is_alive(child) ||
      world->is_alive(grandchild)) {
    return 113;
  }
  if (!world->is_alive(bystander) || !world->is_alive(bystanderChild)) {
    return 114;
  }
  if (world->alive_entity_count() != 2U) {
    return 115;
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

/// for_each_alive must visit every entity that was alive when the walk
/// started even when the callback destroys entities: the loop bound is a
/// snapshot, not the live alive count.
int verify_for_each_alive_visits_all_while_destroying() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 340;
  }

  constexpr std::size_t kEntityCount = 16U;
  for (std::size_t i = 0U; i < kEntityCount; ++i) {
    if (world->create_scene_object() == engine::runtime::kInvalidEntity) {
      return 341;
    }
  }
  if (world->alive_entity_count() != kEntityCount) {
    return 342;
  }

  std::size_t visited = 0U;
  std::size_t destroyed = 0U;
  world->for_each_alive([&](engine::runtime::Entity entity) noexcept {
    ++visited;
    if (world->destroy_entity(entity)) {
      ++destroyed;
    }
  });

  if (visited != kEntityCount) {
    return 343;
  }
  if (destroyed != kEntityCount) {
    return 344;
  }
  if (world->alive_entity_count() != 0U) {
    return 345;
  }
  return 0;
}

int verify_variadic_for_each() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 100;
  }

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

/// EXPECTATION (audit H-06): component and gravity ingress rejects
/// non-finite, non-positive-extent, and negative-mass/material values with
/// the destination unchanged, while valid values keep being accepted.
int verify_physics_ingress_validation() {
  using namespace engine::runtime;
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInf = std::numeric_limits<float>::infinity();

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 900;
  }
  const Entity entity = world->create_entity();
  if (entity == kInvalidEntity) {
    return 901;
  }

  Transform badTransform{};
  badTransform.position.x = kNaN;
  if (world->add_transform(entity, badTransform)) {
    return 902;
  }
  Transform probe{};
  if (world->get_transform(entity, &probe)) {
    return 903;
  }
  Transform goodTransform{};
  if (!world->add_transform(entity, goodTransform)) {
    return 904;
  }

  RigidBody badBody{};
  badBody.velocity.y = kInf;
  if (world->add_rigid_body(entity, badBody)) {
    return 905;
  }
  badBody = RigidBody{};
  badBody.inverseMass = -1.0F;
  if (world->add_rigid_body(entity, badBody)) {
    return 906;
  }
  RigidBody goodBody{};
  goodBody.inverseMass = 0.0F;
  goodBody.inverseInertia = 0.0F;
  if (!world->add_rigid_body(entity, goodBody)) {
    return 907;
  }

  Collider badCollider{};
  badCollider.halfExtents.z = kNaN;
  if (world->add_collider(entity, badCollider)) {
    return 908;
  }
  badCollider = Collider{};
  badCollider.halfExtents.y = 0.0F;
  if (world->add_collider(entity, badCollider)) {
    return 909;
  }
  badCollider = Collider{};
  badCollider.restitution = -0.5F;
  if (world->add_collider(entity, badCollider)) {
    return 910;
  }
  Collider probeCollider{};
  if (world->get_collider(entity, &probeCollider)) {
    return 911;
  }
  if (!world->add_collider(entity, Collider{})) {
    return 912;
  }

  set_gravity(*world, 0.5F, -5.0F, 0.0F);
  set_gravity(*world, kNaN, 0.0F, 0.0F);
  float gx = 0.0F;
  float gy = 0.0F;
  float gz = 0.0F;
  if (!get_gravity(*world, &gx, &gy, &gz) || (gx != 0.5F) || (gy != -5.0F) ||
      (gz != 0.0F)) {
    return 913;
  }

  // Audit M-21/X-1: NaN opacity fails the `opacity < 1` transparency test
  // and reaches the sort key's float-to-uint16_t depth conversion, which is
  // undefined behavior, so mesh material factors validate and clamp too.
  engine::runtime::MeshComponent badMesh{};
  badMesh.opacity = kNaN;
  if (world->add_mesh_component(entity, badMesh)) {
    return 914;
  }
  badMesh = engine::runtime::MeshComponent{};
  badMesh.roughness = kInf;
  if (world->add_mesh_component(entity, badMesh)) {
    return 915;
  }
  badMesh = engine::runtime::MeshComponent{};
  badMesh.albedo.x = kNaN;
  if (world->add_mesh_component(entity, badMesh)) {
    return 916;
  }
  engine::runtime::MeshComponent probeMesh{};
  if (world->get_mesh_component(entity, &probeMesh)) {
    return 917;
  }

  engine::runtime::MeshComponent outOfRange{};
  outOfRange.opacity = 4.0F;
  outOfRange.roughness = -2.0F;
  outOfRange.metallic = 8.0F;
  outOfRange.albedo = engine::math::Vec3(-1.0F, 3.0F, 0.5F);
  if (!world->add_mesh_component(entity, outOfRange)) {
    return 918;
  }
  engine::runtime::MeshComponent storedMesh{};
  if (!world->get_mesh_component(entity, &storedMesh)) {
    return 919;
  }
  if ((storedMesh.opacity != 1.0F) || (storedMesh.roughness != 0.0F) ||
      (storedMesh.metallic != 1.0F) || (storedMesh.albedo.x != 0.0F) ||
      (storedMesh.albedo.y != 1.0F) || (storedMesh.albedo.z != 0.5F)) {
    return 920;
  }

  return 0;
}

/// EXPECTATION (audit P-5, H-06 remainder): finite but out-of-envelope
/// material and velocity values are accepted with a clamp instead of a
/// silent pass-through: restitution clamps to 1 (max-combined and
/// multiplied into approach speed, so e > 1 injects energy per bounce),
/// dynamic friction clamps down to static (sliding force must not exceed
/// the stick threshold), inverse inertia clamps to kMaxInverseInertia,
/// and linear/angular velocity clamp to kMaxLinearSpeed/kMaxAngularSpeed
/// both at ingress and after integration. Clamp factors in the cases
/// below are exactly representable (0.5), so stored values are asserted
/// exactly; boundary values at the caps must pass through unchanged.
int verify_physics_ingress_clamps() {
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 920;
  }

  const Entity entity = world->create_entity();
  Transform transform{};
  if ((entity == kInvalidEntity) || !world->add_transform(entity, transform)) {
    return 921;
  }

  Collider hotCollider{};
  hotCollider.restitution = 1.5F;
  hotCollider.staticFriction = 0.2F;
  hotCollider.dynamicFriction = 0.9F;
  Collider stored{};
  if (!world->add_collider(entity, hotCollider) ||
      !world->get_collider(entity, &stored) || (stored.restitution != 1.0F) ||
      (stored.dynamicFriction != 0.2F) || (stored.staticFriction != 0.2F)) {
    return 922;
  }
  if (!world->remove_collider(entity)) {
    return 923;
  }

  Collider boundaryCollider{};
  boundaryCollider.restitution = 1.0F;
  boundaryCollider.staticFriction = 0.5F;
  boundaryCollider.dynamicFriction = 0.5F;
  if (!world->add_collider(entity, boundaryCollider) ||
      !world->get_collider(entity, &stored) || (stored.restitution != 1.0F) ||
      (stored.dynamicFriction != 0.5F)) {
    return 924;
  }

  RigidBody hotBody{};
  hotBody.inverseMass = 1.0F;
  hotBody.inverseInertia = 1.0e6F;
  hotBody.velocity = engine::math::Vec3(600.0F, 800.0F, 0.0F);
  hotBody.angularVelocity = engine::math::Vec3(0.0F, 24.0F, 0.0F);
  RigidBody storedBody{};
  if (!world->add_rigid_body(entity, hotBody) ||
      !world->get_rigid_body(entity, &storedBody) ||
      (storedBody.inverseInertia != engine::physics::kMaxInverseInertia) ||
      (storedBody.velocity.x != 300.0F) || (storedBody.velocity.y != 400.0F) ||
      (storedBody.velocity.z != 0.0F) ||
      (storedBody.angularVelocity.y != 12.0F)) {
    return 925;
  }
  if (!world->remove_rigid_body(entity)) {
    return 926;
  }

  RigidBody boundaryBody{};
  boundaryBody.inverseMass = 1.0F;
  boundaryBody.inverseInertia = engine::physics::kMaxInverseInertia;
  boundaryBody.velocity =
      engine::math::Vec3(0.0F, 0.0F, engine::physics::kMaxLinearSpeed);
  if (!world->add_rigid_body(entity, boundaryBody) ||
      !world->get_rigid_body(entity, &storedBody) ||
      (storedBody.velocity.z != engine::physics::kMaxLinearSpeed) ||
      (storedBody.inverseInertia != engine::physics::kMaxInverseInertia)) {
    return 927;
  }

  set_gravity(*world, 0.0F, 0.0F, 0.0F);
  {
    RigidBody *live = world->get_rigid_body_ptr(entity);
    if (live == nullptr) {
      return 928;
    }
    live->velocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
    live->acceleration = engine::math::Vec3(0.0F, 0.0F, 60000.0F);
  }
  world->begin_update_phase();
  if (!step_physics(*world, 1.0F / 60.0F)) {
    return 929;
  }
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  if (!world->get_rigid_body(entity, &storedBody) ||
      (storedBody.velocity.z != engine::physics::kMaxLinearSpeed)) {
    return 930;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
/// EXPECTATION (audit H-18): reset_world is a full destructive reset in
/// any phase — called mid-Simulation it destroys every entity
/// immediately (the old path deferred them, leaving live components over
/// cleared managers and a reset controller registry) and drops the
/// pending deferred-destroy queue so completing the frame cannot fire a
/// stale destroy into replacement content, which stays creatable.
int verify_reset_world_phase_independent() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 950;
  }
  world->end_frame_phase();

  const auto first = world->create_scene_object();
  const auto second = world->create_scene_object();
  const auto third = world->create_scene_object();
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity) ||
      (third == engine::runtime::kInvalidEntity)) {
    return 951;
  }

  world->begin_update_phase();
  // Queues a deferred destroy; the entity must stay alive until a flush.
  if (!world->destroy_entity(first) || !world->is_alive(first)) {
    return 952;
  }

  engine::runtime::reset_world(*world);
  if (world->alive_entity_count() != 0U) {
    return 953;
  }
  if (world->is_alive(first) || world->is_alive(second) ||
      world->is_alive(third)) {
    return 954;
  }

  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->begin_render_phase();
  world->end_frame_phase();
  if (world->alive_entity_count() != 0U) {
    return 955;
  }

  const auto replacement = world->create_scene_object();
  if ((replacement == engine::runtime::kInvalidEntity) ||
      (world->alive_entity_count() != 1U)) {
    return 956;
  }
  return 0;
}

int main() {
  int result = verify_raw_and_scene_object_creation();
  if (result != 0) {
    return result;
  }

  result = verify_parent_trs_propagation_exact();
  if (result != 0) {
    return result;
  }

  result = verify_persistent_id_index();
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

  result = verify_cascade_destroy_subtree();
  if (result != 0) {
    return result;
  }

  result = verify_physics_ingress_validation();
  if (result != 0) {
    return result;
  }

  result = verify_physics_ingress_clamps();
  if (result != 0) {
    return result;
  }

  result = verify_reset_world_phase_independent();
  if (result != 0) {
    return result;
  }

  result = verify_for_each_alive_visits_all_while_destroying();
  if (result != 0) {
    return result;
  }

  return verify_variadic_for_each();
}
