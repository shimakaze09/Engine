// Verifies hierarchy-aware collider transforms and compound-body invariants.

#include "engine/math/component_types.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/physics_types.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

std::size_t g_pairCount = 0U;
engine::runtime::Entity g_pairEntityA{};
engine::runtime::Entity g_pairEntityB{};

// Records the first collision pair so tests can assert exact entity ownership.
void record_collision_pairs(const engine::runtime::Entity *pairs,
                            std::size_t pairCount) noexcept {
  g_pairCount = pairCount;
  g_pairEntityA = (pairCount > 0U) ? pairs[0] : engine::runtime::Entity{};
  g_pairEntityB = (pairCount > 0U) ? pairs[1] : engine::runtime::Entity{};
}

// Clears callback state before resolving an isolated test world.
void reset_collision_pairs() noexcept {
  g_pairCount = 0U;
  g_pairEntityA = {};
  g_pairEntityB = {};
}

// Reports whether the callback's first pair contains the requested entities
// (full identity: index and generation).
[[nodiscard]] bool recorded_pair(engine::runtime::Entity first,
                                 engine::runtime::Entity second) noexcept {
  return g_pairCount == 1U &&
         ((g_pairEntityA == first && g_pairEntityB == second) ||
          (g_pairEntityA == second && g_pairEntityB == first));
}

// Compares vectors exactly; all test transforms use binary-exact values.
[[nodiscard]] bool equal(const engine::math::Vec3 &left,
                         const engine::math::Vec3 &right) noexcept {
  return left.x == right.x && left.y == right.y && left.z == right.z;
}

// Creates a World in its component-mutation phase.
[[nodiscard]] std::unique_ptr<engine::runtime::World> make_world() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world != nullptr) {
    world->end_frame_phase();
  }
  return world;
}

// Runs hierarchy composition and collision resolution through a full frame.
[[nodiscard]] bool resolve_frame(engine::runtime::World &world) noexcept {
  world.begin_update_phase();
  if (!world.update_transforms_range(0U, world.transform_count(), 0.0F) ||
      !engine::runtime::resolve_collisions(world)) {
    world.end_frame_phase();
    return false;
  }
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
  engine::runtime::dispatch_collision_callbacks(world);
  return true;
}

// Confirms a rotated, nonuniformly scaled box participates at its world bounds.
[[nodiscard]] bool test_rotated_scaled_box_collision() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return false;
  }

  engine::runtime::Transform scaledTransform{};
  scaledTransform.rotation =
      engine::math::Quat(0.0F, 0.0F, 0.382683432F, 0.923879533F);
  scaledTransform.scale = engine::math::Vec3(4.0F, 1.0F, 1.0F);
  const engine::runtime::Entity scaledBox =
      world->create_scene_object(scaledTransform);

  engine::runtime::Transform movingTransform{};
  movingTransform.position = engine::math::Vec3(0.0F, 1.5F, 0.125F);
  const engine::runtime::Entity movingBox =
      world->create_scene_object(movingTransform);
  if (scaledBox == engine::runtime::kInvalidEntity ||
      movingBox == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::RigidBody dynamicBody{};
  dynamicBody.inverseMass = 1.0F;
  if (!world->add_collider(scaledBox, collider) ||
      !world->add_collider(movingBox, collider) ||
      !world->add_rigid_body(movingBox, dynamicBody)) {
    return false;
  }

  reset_collision_pairs();
  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);
  const bool resolved = resolve_frame(*world);
  if (!resolved || !recorded_pair(scaledBox, movingBox)) {
    std::fprintf(
        stderr,
        "rotated box details: resolved=%d pairs=%zu first=%u second=%u\n",
        resolved ? 1 : 0, g_pairCount, g_pairEntityA.index, g_pairEntityB.index);
    return false;
  }

  engine::runtime::Transform moved{};
  const bool hasMovedTransform = world->get_transform(movingBox, &moved);
  if (!hasMovedTransform || !(moved.position.y > movingTransform.position.y)) {
    std::fprintf(stderr,
                 "rotated box position: has=%d y=%f expected-above=%f\n",
                 hasMovedTransform ? 1 : 0, moved.position.y,
                 movingTransform.position.y);
    return false;
  }
  return true;
}

// Confirms a child collider inherits its parent's complete affine transform.
[[nodiscard]] bool test_child_collider_follows_parent_trs() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return false;
  }

  engine::runtime::Transform parentTransform{};
  parentTransform.position = engine::math::Vec3(10.0F, 20.0F, 30.0F);
  parentTransform.rotation = engine::math::Quat(0.0F, 0.0F, 1.0F, 0.0F);
  parentTransform.scale = engine::math::Vec3(2.0F, 3.0F, 4.0F);
  const engine::runtime::Entity parent =
      world->create_scene_object(parentTransform);
  if (parent == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Transform childTransform{};
  childTransform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  childTransform.scale = engine::math::Vec3(0.5F, 2.0F, 0.25F);
  childTransform.parentId = world->persistent_id(parent);
  const engine::runtime::Entity child =
      world->create_scene_object(childTransform);
  if (child == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Collider collider{};
  collider.localPosition = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  collider.localRotation = engine::math::Quat(1.0F, 0.0F, 0.0F, 0.0F);
  collider.halfExtents = engine::math::Vec3(1.0F, 0.5F, 2.0F);
  if (!world->add_collider(child, collider)) {
    return false;
  }

  world->begin_update_phase();
  world->commit_update_phase();
  world->begin_render_prep_phase();
  engine::physics::PhysicsTransform childWorld{};
  const bool hasTransform = world->get_physics_transform(child, &childWorld);
  world->end_frame_phase();
  if (!hasTransform ||
      !equal(childWorld.position, engine::math::Vec3(8.0F, 14.0F, 42.0F)) ||
      !equal(childWorld.scale, engine::math::Vec3(1.0F, 6.0F, 1.0F))) {
    return false;
  }

  engine::physics::ColliderWorldGeometry geometry{};
  return engine::physics::make_collider_world_geometry(
             collider, childWorld.matrix, nullptr, &geometry) &&
         equal(geometry.center, engine::math::Vec3(7.0F, 8.0F, 43.0F)) &&
         equal(geometry.worldAabb.min, engine::math::Vec3(6.0F, 5.0F, 41.0F)) &&
         equal(geometry.worldAabb.max, engine::math::Vec3(8.0F, 11.0F, 45.0F));
}

// Confirms child contact correction is applied to its nearest body ancestor.
[[nodiscard]] bool test_child_contact_moves_compound_root() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return false;
  }

  const engine::runtime::Entity root = world->create_scene_object();
  if (root == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::RigidBody rootBody{};
  rootBody.inverseMass = 1.0F;
  if (!world->add_rigid_body(root, rootBody)) {
    return false;
  }

  engine::runtime::Transform childTransform{};
  childTransform.parentId = world->persistent_id(root);
  const engine::runtime::Entity child =
      world->create_scene_object(childTransform);
  engine::runtime::Transform obstacleTransform{};
  obstacleTransform.position = engine::math::Vec3(0.75F, 0.125F, 0.0625F);
  const engine::runtime::Entity obstacle =
      world->create_scene_object(obstacleTransform);
  if (child == engine::runtime::kInvalidEntity ||
      obstacle == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_collider(child, collider) ||
      !world->add_collider(obstacle, collider)) {
    return false;
  }

  reset_collision_pairs();
  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);
  const bool resolved = resolve_frame(*world);
  if (!resolved || !recorded_pair(child, obstacle)) {
    std::fprintf(stderr,
                 "compound details: resolved=%d pairs=%zu first=%u second=%u\n",
                 resolved ? 1 : 0, g_pairCount, g_pairEntityA.index,
                 g_pairEntityB.index);
    return false;
  }

  engine::runtime::Transform rootAfter{};
  engine::runtime::Transform childAfter{};
  const bool hasRoot = world->get_transform(root, &rootAfter);
  const bool hasChild = world->get_transform(child, &childAfter);
  if (!hasRoot || !hasChild || !(rootAfter.position.x < 0.0F) ||
      !equal(childAfter.position, engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    std::fprintf(
        stderr,
        "compound movement: root=%d child=%d root-x=%f child=(%f,%f,%f)\n",
        hasRoot ? 1 : 0, hasChild ? 1 : 0, rootAfter.position.x,
        childAfter.position.x, childAfter.position.y, childAfter.position.z);
    return false;
  }
  return true;
}

// Confirms overlapping colliders owned by one body never contact each other.
[[nodiscard]] bool test_same_root_colliders_do_not_self_collide() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return false;
  }

  const engine::runtime::Entity root = world->create_scene_object();
  if (root == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::RigidBody rootBody{};
  rootBody.inverseMass = 1.0F;
  if (!world->add_rigid_body(root, rootBody)) {
    return false;
  }

  engine::runtime::Transform childTransform{};
  childTransform.parentId = world->persistent_id(root);
  const engine::runtime::Entity firstChild =
      world->create_scene_object(childTransform);
  const engine::runtime::Entity secondChild =
      world->create_scene_object(childTransform);
  if (firstChild == engine::runtime::kInvalidEntity ||
      secondChild == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  if (!world->add_collider(firstChild, collider) ||
      !world->add_collider(secondChild, collider)) {
    return false;
  }

  reset_collision_pairs();
  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);
  if (!resolve_frame(*world) || g_pairCount != 0U) {
    return false;
  }

  engine::runtime::Transform rootAfter{};
  engine::runtime::RigidBody bodyAfter{};
  return world->get_transform(root, &rootAfter) &&
         world->get_rigid_body(root, &bodyAfter) &&
         equal(rootAfter.position, engine::math::Vec3(0.0F, 0.0F, 0.0F)) &&
         equal(bodyAfter.velocity, engine::math::Vec3(0.0F, 0.0F, 0.0F)) &&
         equal(bodyAfter.angularVelocity, engine::math::Vec3(0.0F, 0.0F, 0.0F));
}

// Confirms both component insertion orders reject parented dynamic bodies.
[[nodiscard]] bool test_dynamic_rigid_body_parenting_rejected() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    return false;
  }

  const engine::runtime::Entity parent = world->create_scene_object();
  if (parent == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::Transform parentedTransform{};
  parentedTransform.parentId = world->persistent_id(parent);
  const engine::runtime::Entity parented =
      world->create_scene_object(parentedTransform);
  if (parented == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::RigidBody dynamicBody{};
  dynamicBody.inverseMass = 1.0F;
  if (world->add_rigid_body(parented, dynamicBody)) {
    return false;
  }
  engine::runtime::RigidBody rejectedBody{};
  if (world->get_rigid_body(parented, &rejectedBody)) {
    return false;
  }

  const engine::runtime::Entity bodyFirst = world->create_entity();
  if (bodyFirst == engine::runtime::kInvalidEntity ||
      !world->add_rigid_body(bodyFirst, dynamicBody) ||
      world->add_transform(bodyFirst, parentedTransform)) {
    return false;
  }
  engine::runtime::Transform rejectedTransform{};
  return !world->get_transform(bodyFirst, &rejectedTransform);
}

using TestFunction = bool (*)() noexcept;

// Names an invariant test for concise failure output.
struct TestCase final {
  const char *name;
  TestFunction function;
};

constexpr TestCase kTests[] = {
    {"rotated_scaled_box_collision", &test_rotated_scaled_box_collision},
    {"child_collider_follows_parent_trs",
     &test_child_collider_follows_parent_trs},
    {"child_contact_moves_compound_root",
     &test_child_contact_moves_compound_root},
    {"same_root_colliders_do_not_self_collide",
     &test_same_root_colliders_do_not_self_collide},
    {"dynamic_rigid_body_parenting_rejected",
     &test_dynamic_rigid_body_parenting_rejected},
};

} // namespace

int main() {
  int failures = 0;
  for (const TestCase &test : kTests) {
    if (!test.function()) {
      std::fprintf(stderr, "physics hierarchy collision test failed: %s\n",
                   test.name);
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
