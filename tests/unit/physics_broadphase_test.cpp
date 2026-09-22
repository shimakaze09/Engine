// Verifies the collision broadphase through resolve_collisions (#536): pair
// testing is held to the pairs whose travel-grown bounds overlap, however
// large one collider is, and a pair that cannot touch within the step
// receives no impulse, whichever grid cell the two happen to share.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

using engine::runtime::World;

std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
    engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);
  }
  return world;
}

bool run_resolve(World &world) noexcept {
  world.begin_update_phase();
  const bool resolved = engine::runtime::resolve_collisions(world);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
  return resolved;
}

/// A static collider (no rigid body) centred at `position`.
bool add_static_box(World &world, const engine::math::Vec3 &position,
                    const engine::math::Vec3 &halfExtents) noexcept {
  const auto entity = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = position;
  engine::runtime::Collider collider{};
  collider.halfExtents = halfExtents;
  return world.add_transform(entity, transform) &&
         world.add_collider(entity, collider);
}

/// A dynamic body with one collider of `shape`.
engine::runtime::Entity add_body(World &world,
                                 const engine::math::Vec3 &position,
                                 const engine::math::Vec3 &halfExtents,
                                 engine::runtime::ColliderShape shape,
                                 const engine::math::Vec3 &velocity) noexcept {
  const auto entity = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = position;
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  body.velocity = velocity;
  engine::runtime::Collider collider{};
  collider.shape = shape;
  collider.halfExtents = halfExtents;
  if (!world.add_transform(entity, transform) ||
      !world.add_rigid_body(entity, body) ||
      !world.add_collider(entity, collider)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

/// One ground box 28 m across under 100 small boxes, each resting into
/// it and 2.5 m from its neighbours: the overlapping pairs are the 100
/// box-ground pairs, and only they may reach a narrow phase. The grid's
/// cell was sized from the largest collider, so the ground put every
/// collider in one cell and all 5,050 pairs were tested.
int check_large_ground_keeps_pair_tests_to_overlaps() {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 10;
  }
  if (!add_static_box(*world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                      engine::math::Vec3(14.0F, 0.5F, 14.0F))) {
    return 11;
  }
  constexpr int kSide = 10;
  for (int x = 0; x < kSide; ++x) {
    for (int z = 0; z < kSide; ++z) {
      const engine::math::Vec3 position(
          -11.25F + (2.5F * static_cast<float>(x)), 0.74F,
          -11.25F + (2.5F * static_cast<float>(z)));
      if (add_body(*world, position, engine::math::Vec3(0.25F, 0.25F, 0.25F),
                   engine::runtime::ColliderShape::AABB,
                   engine::math::Vec3(0.0F, 0.0F, 0.0F)) ==
          engine::runtime::kInvalidEntity) {
        return 12;
      }
    }
  }

  if (!run_resolve(*world)) {
    return 13;
  }
  const std::uint32_t tested = world->physics_context().narrowPhasePairTests;
  if (tested != static_cast<std::uint32_t>(kSide * kSide)) {
    std::printf("%u pairs reached a narrow phase; 100 overlap\n", tested);
    return 14;
  }
  return 0;
}

/// Sphere B passes sphere A at 100 m/s along -Y, 0.1 m clear of it on X:
/// over one step neither's bounds, grown by its travel, reach the other's,
/// so they cannot touch. The speculative sphere test used to fire anyway
/// -- the closing speed along the current centre line exceeds the gap
/// over one step -- and pushed B off its path. A ground box far away is
/// present so the two share one grid cell whichever way the grid is sized.
int check_passing_bodies_receive_no_impulse() {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    return 20;
  }
  if (!add_static_box(*world, engine::math::Vec3(0.0F, -40.0F, 0.0F),
                      engine::math::Vec3(14.0F, 0.5F, 14.0F))) {
    return 21;
  }
  const auto sphereA = add_body(*world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(1.0F, 1.0F, 1.0F),
                                engine::runtime::ColliderShape::Sphere,
                                engine::math::Vec3(0.0F, 0.0F, 0.0F));
  const auto sphereB = add_body(*world, engine::math::Vec3(2.1F, 2.1F, 0.0F),
                                engine::math::Vec3(1.0F, 1.0F, 1.0F),
                                engine::runtime::ColliderShape::Sphere,
                                engine::math::Vec3(0.0F, -100.0F, 0.0F));
  if ((sphereA == engine::runtime::kInvalidEntity) ||
      (sphereB == engine::runtime::kInvalidEntity)) {
    return 22;
  }

  if (!run_resolve(*world)) {
    return 23;
  }
  const engine::runtime::RigidBody *a = world->get_rigid_body_ptr(sphereA);
  const engine::runtime::RigidBody *b = world->get_rigid_body_ptr(sphereB);
  if ((a == nullptr) || (b == nullptr)) {
    return 24;
  }
  // Exact: no impulse means the velocities are the values written above.
  if ((a->velocity.x != 0.0F) || (a->velocity.y != 0.0F) ||
      (a->velocity.z != 0.0F) || (b->velocity.x != 0.0F) ||
      (b->velocity.y != -100.0F) || (b->velocity.z != 0.0F)) {
    std::printf(
        "A (%g, %g, %g) B (%g, %g, %g)\n", static_cast<double>(a->velocity.x),
        static_cast<double>(a->velocity.y), static_cast<double>(a->velocity.z),
        static_cast<double>(b->velocity.x), static_cast<double>(b->velocity.y),
        static_cast<double>(b->velocity.z));
    return 25;
  }
  return 0;
}

} // namespace

int main() {
  int result = check_passing_bodies_receive_no_impulse();
  if (result == 0) {
    result = check_large_ground_keeps_pair_tests_to_overlaps();
  }
  if (result != 0) {
    std::fprintf(stderr, "physics_broadphase_test failed: %d\n", result);
  }
  return result;
}
