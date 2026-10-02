// Verifies the layer collision matrix through the production pair filter:
// - the default matrix filters nothing beyond the colliders' own masks;
// - an ignored pair neither collides, nor reports a trigger overlap, nor
//   stops a CCD sweep, while the same bodies on other layers still do;
// - a layer may ignore itself, and a collider on several layers collides
//   through any of them;
// - changing the matrix wakes sleeping bodies, an unchanged one does not;
// - a content copy (a scene load's commit, an editor Stop restore) keeps
//   the live world's matrix;
// - the runtime builds the matrix from the project's layers and refuses to
//   change it while the world simulates.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <vector>

#include "engine/content/project_document.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/runtime/collision_layers.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

using engine::math::Vec3;
using engine::physics::CollisionLayerMatrix;
using engine::runtime::World;

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

constexpr std::uint32_t bit(std::uint32_t index) noexcept {
  return 1U << index;
}

/// A matrix with each listed pair ignored, both ways round.
CollisionLayerMatrix
ignoring(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> pairs) {
  CollisionLayerMatrix matrix{};
  for (const auto &[a, b] : pairs) {
    matrix.rows[a] &= ~bit(b);
    matrix.rows[b] &= ~bit(a);
  }
  return matrix;
}

std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
    engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);
  }
  return world;
}

engine::runtime::Entity add_box(World &world, float x, std::uint32_t layer,
                                float inverseMass,
                                bool isTrigger = false) noexcept {
  const engine::runtime::Entity entity = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = Vec3(x, 0.0F, 0.0F);
  engine::runtime::Collider collider{};
  collider.halfExtents = Vec3(0.5F, 0.5F, 0.5F);
  collider.collisionLayer = layer;
  collider.isTrigger = isTrigger;
  engine::runtime::RigidBody body{};
  body.inverseMass = inverseMass;
  static_cast<void>(world.add_transform(entity, transform));
  static_cast<void>(world.add_collider(entity, collider));
  if (!isTrigger) {
    static_cast<void>(world.add_rigid_body(entity, body));
  }
  return entity;
}

void resolve_once(World &world) noexcept {
  world.begin_update_phase();
  engine::runtime::resolve_collisions(world);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
}

/// Whether two overlapping boxes on layers `a` and `b` are pushed apart
/// under `matrix`.
bool boxes_collide(std::uint32_t a, std::uint32_t b,
                   const CollisionLayerMatrix &matrix) noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return false;
  }
  engine::physics::set_collision_matrix(*world, matrix);
  const engine::runtime::Entity first = add_box(*world, 0.0F, a, 1.0F);
  const engine::runtime::Entity second = add_box(*world, 0.5F, b, 1.0F);
  resolve_once(*world);
  engine::runtime::Transform ta{};
  engine::runtime::Transform tb{};
  static_cast<void>(world->get_transform(first, &ta));
  static_cast<void>(world->get_transform(second, &tb));
  return std::fabs(tb.position.x - ta.position.x) > 0.5F;
}

void check_pair_filter() {
  const CollisionLayerMatrix all{};
  check(boxes_collide(bit(1), bit(2), all),
        "the default matrix lets every pair collide");
  const CollisionLayerMatrix noOneTwo = ignoring({{1U, 2U}});
  check(!boxes_collide(bit(1), bit(2), noOneTwo) &&
            !boxes_collide(bit(2), bit(1), noOneTwo),
        "an ignored pair does not collide, whichever body comes first");
  check(boxes_collide(bit(1), bit(3), noOneTwo) &&
            boxes_collide(bit(2), bit(2), noOneTwo),
        "the same matrix still lets other pairs collide");
  check(!boxes_collide(bit(4), bit(4), ignoring({{4U, 4U}})),
        "a layer may ignore itself");
  check(boxes_collide(bit(1) | bit(3), bit(2), noOneTwo),
        "a collider on several layers collides through any of them");
  check(!boxes_collide(bit(1) | bit(3), bit(2), ignoring({{1U, 2U}, {3U, 2U}})),
        "it stops colliding once every one of its layers ignores the other");
}

void check_trigger_filter() {
  for (const bool ignored : {false, true}) {
    std::unique_ptr<World> world = make_world();
    check(world != nullptr, "trigger: world");
    if (world == nullptr) {
      return;
    }
    engine::physics::set_collision_matrix(
        *world, ignored ? ignoring({{5U, 6U}}) : CollisionLayerMatrix{});
    static_cast<void>(add_box(*world, 0.0F, bit(5), 0.0F, true));
    static_cast<void>(add_box(*world, 0.5F, bit(6), 1.0F));
    resolve_once(*world);
    const std::size_t overlaps =
        world->physics_context().shapeStore->triggerOverlapCount;
    check(overlaps == (ignored ? 0U : 1U),
          ignored ? "a trigger on an ignored pair reports no overlap"
                  : "a trigger on an allowed pair reports its overlap");
  }
}

/// A 0.1 m sphere fired at 300 m/s at a 4 cm wall 5 m away, one step:
/// where the bullet ends.
float bullet_x(const CollisionLayerMatrix &matrix) noexcept {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 0.0F;
  }
  engine::physics::set_collision_matrix(*world, matrix);
  const engine::runtime::Entity bullet = world->create_entity();
  engine::runtime::Transform bulletTransform{};
  engine::runtime::Collider bulletCollider{};
  bulletCollider.shape = engine::runtime::ColliderShape::Sphere;
  bulletCollider.halfExtents = Vec3(0.1F, 0.1F, 0.1F);
  bulletCollider.collisionLayer = bit(1);
  engine::runtime::RigidBody bulletBody{};
  bulletBody.inverseMass = 1.0F;
  bulletBody.velocity = Vec3(300.0F, 0.0F, 0.0F);
  static_cast<void>(world->add_transform(bullet, bulletTransform));
  static_cast<void>(world->add_collider(bullet, bulletCollider));
  static_cast<void>(world->add_rigid_body(bullet, bulletBody));

  const engine::runtime::Entity wall = world->create_entity();
  engine::runtime::Transform wallTransform{};
  wallTransform.position = Vec3(5.0F, 0.0F, 0.0F);
  engine::runtime::Collider wallCollider{};
  wallCollider.halfExtents = Vec3(0.02F, 2.0F, 2.0F);
  wallCollider.collisionLayer = bit(2);
  engine::runtime::RigidBody wallBody{};
  wallBody.inverseMass = 0.0F;
  static_cast<void>(world->add_transform(wall, wallTransform));
  static_cast<void>(world->add_collider(wall, wallCollider));
  static_cast<void>(world->add_rigid_body(wall, wallBody));

  world->begin_update_phase();
  engine::runtime::step_physics(*world, 1.0F / 60.0F);
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  engine::runtime::Transform result{};
  static_cast<void>(world->get_transform(bullet, &result));
  return result.position.x;
}

void check_ccd_filter() {
  check(bullet_x(CollisionLayerMatrix{}) < 5.0F,
        "CCD stops a bullet at a wall on an allowed pair");
  check(bullet_x(ignoring({{1U, 2U}})) > 5.0F,
        "CCD lets a bullet through a wall on an ignored pair");
}

void check_wake() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "wake: world");
  if (world == nullptr) {
    return;
  }
  const engine::runtime::Entity body = add_box(*world, 0.0F, bit(0), 1.0F);
  auto put_to_sleep = [&] {
    engine::runtime::RigidBody *rb = world->get_rigid_body_ptr(body);
    if (rb != nullptr) {
      rb->sleeping = true;
    }
  };
  auto asleep = [&] {
    const engine::runtime::RigidBody *rb = world->get_rigid_body_ptr(body);
    return (rb != nullptr) && rb->sleeping;
  };
  put_to_sleep();
  engine::physics::set_collision_matrix(*world, CollisionLayerMatrix{});
  check(asleep(), "installing the same matrix leaves bodies asleep");
  engine::physics::set_collision_matrix(*world, ignoring({{0U, 1U}}));
  check(!asleep(), "changing the matrix wakes sleeping bodies");
}

void check_content_copy_keeps_matrix() {
  std::unique_ptr<World> live = make_world();
  std::unique_ptr<World> staged = make_world();
  check((live != nullptr) && (staged != nullptr), "copy: worlds");
  if ((live == nullptr) || (staged == nullptr)) {
    return;
  }
  const CollisionLayerMatrix project = ignoring({{1U, 2U}});
  engine::physics::set_collision_matrix(*live, project);
  *live = *staged;
  check(engine::physics::get_collision_matrix(*live).rows == project.rows,
        "assigning a staged world over the live one keeps its matrix");

  static_cast<void>(add_box(*live, 0.0F, bit(1), 1.0F));
  std::vector<char> buffer(64U * 1024U);
  std::size_t size = 0U;
  check(
      engine::runtime::save_scene(*live, buffer.data(), buffer.size(), &size) &&
          engine::runtime::load_scene(*live, buffer.data(), size),
      "copy: a scene saves and loads back");
  check(engine::physics::get_collision_matrix(*live).rows == project.rows,
        "a scene load keeps the live world's matrix");
}

void check_runtime_layers() {
  engine::content::ProjectCollisionLayers layers{};
  engine::content::set_collision_layer_pair(&layers, 3U, 7U, false);
  const CollisionLayerMatrix matrix =
      engine::runtime::collision_matrix_from(layers);
  check((matrix.rows[3] == ~bit(7)) && (matrix.rows[7] == ~bit(3)) &&
            (matrix.rows[0] == 0xFFFFFFFFU),
        "the runtime matrix mirrors the project's rows");

  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "runtime: world");
  if (world == nullptr) {
    return;
  }
  engine::runtime::set_project_collision_layers(layers);
  world->begin_update_phase();
  check(!engine::runtime::apply_project_collision_layers(*world) &&
            (engine::physics::get_collision_matrix(*world).rows ==
             CollisionLayerMatrix{}.rows),
        "the matrix cannot change while the world simulates");
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  check(engine::runtime::apply_project_collision_layers(*world) &&
            (engine::physics::get_collision_matrix(*world).rows == matrix.rows),
        "outside a step the project's matrix is installed");
  engine::runtime::set_project_collision_layers(
      engine::content::ProjectCollisionLayers{});
}

} // namespace

int main() {
  check_pair_filter();
  check_trigger_filter();
  check_ccd_filter();
  check_wake();
  check_content_copy_keeps_matrix();
  check_runtime_layers();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d collision layer matrix check(s) failed\n",
                 g_failures);
    return 1;
  }
  return 0;
}
