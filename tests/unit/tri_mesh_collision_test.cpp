// Verifies bodies collide with a TriMesh collider through the production
// physics step: a sphere, an upright and a lying capsule, and a tilted box
// dropped onto a triangle-mesh floor come to rest on it, at the heights
// the box-floor rest suite holds an analytic floor to; a mesh on a moving
// body collides with nothing; and a TriMesh collider whose mesh is not
// installed is not a floor. Raycasts, sweeps and overlaps meet the mesh's
// triangles, not its bounds, and a character walks on it and stops at a
// wall made of triangles.

#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/character_move.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/physics_query.h"
#include "engine/physics/tri_mesh.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

namespace {

namespace math = engine::math;
namespace physics = engine::physics;
namespace runtime = engine::runtime;

engine::tests::TestContext g_tests{};

constexpr float kDt = 1.0F / 60.0F;
constexpr int kSettleSteps = 360;

/// Advances one fixed step in the runtime pipeline's phase order.
bool step_world(runtime::World &world) {
  world.begin_update_phase();
  const bool ok = runtime::step_physics(world, kDt) &&
                  runtime::resolve_collisions(world, kDt);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
  return ok;
}

/// A flat floor of `n` x `n` unit quads centred on the origin at y = 0,
/// each quad split into two triangles, so bodies rest across triangle
/// edges as well as inside them.
physics::TriMeshRef make_floor(std::uint32_t n) {
  std::vector<math::Vec3> vertices{};
  std::vector<std::uint32_t> indices{};
  const float half = static_cast<float>(n) * 0.5F;
  for (std::uint32_t z = 0U; z <= n; ++z) {
    for (std::uint32_t x = 0U; x <= n; ++x) {
      vertices.emplace_back(static_cast<float>(x) - half, 0.0F,
                            static_cast<float>(z) - half);
    }
  }
  for (std::uint32_t z = 0U; z < n; ++z) {
    for (std::uint32_t x = 0U; x < n; ++x) {
      const std::uint32_t a = (z * (n + 1U)) + x;
      const std::uint32_t b = a + 1U;
      const std::uint32_t c = a + n + 1U;
      const std::uint32_t d = c + 1U;
      indices.insert(indices.end(), {a, c, b, b, c, d});
    }
  }
  physics::TriMeshRef mesh{};
  static_cast<void>(physics::build_tri_mesh(
      vertices.data(), vertices.size(), indices.data(), indices.size(), &mesh));
  return mesh;
}

std::unique_ptr<runtime::World> make_world() {
  std::unique_ptr<runtime::World> world(new (std::nothrow) runtime::World());
  if (world != nullptr) {
    world->end_frame_phase();
  }
  return world;
}

/// A static TriMesh floor; `install` false leaves its mesh out.
runtime::Entity add_floor(runtime::World &world, bool install) {
  const runtime::Entity floor = world.create_scene_object(runtime::Transform{});
  runtime::Collider collider{};
  collider.shape = runtime::ColliderShape::TriMesh;
  collider.staticFriction = 0.9F;
  collider.dynamicFriction = 0.7F;
  collider.restitution = 0.0F;
  const bool added =
      (floor != runtime::kInvalidEntity) &&
      world.add_collider(floor, collider) &&
      (!install || runtime::set_tri_mesh_data(world, floor, make_floor(20U)));
  g_tests.check(added, "the TriMesh floor is created");
  return floor;
}

runtime::Entity drop(runtime::World &world, const runtime::Collider &collider,
                     const math::Vec3 &position, const math::Quat &rotation) {
  runtime::Transform transform{};
  transform.position = position;
  transform.rotation = rotation;
  const runtime::Entity body = world.create_scene_object(transform);
  runtime::RigidBody rigidBody{};
  rigidBody.inverseMass = 1.0F;
  rigidBody.acceleration = math::Vec3(0.0F, -9.8F, 0.0F);
  runtime::Collider c = collider;
  c.restitution = 0.0F;
  c.staticFriction = 0.9F;
  c.dynamicFriction = 0.7F;
  const bool added = (body != runtime::kInvalidEntity) &&
                     world.add_collider(body, c) &&
                     world.add_rigid_body(body, rigidBody);
  g_tests.check(added, "the dropped body is created");
  return body;
}

runtime::Transform settle(runtime::World &world, runtime::Entity body) {
  bool stepped = true;
  for (int i = 0; i < kSettleSteps; ++i) {
    stepped = step_world(world) && stepped;
  }
  g_tests.check(stepped, "every step runs");
  const runtime::Transform *transform = world.get_transform_read_ptr(body);
  return (transform != nullptr) ? *transform : runtime::Transform{};
}

float speed(const runtime::World &world, runtime::Entity body) {
  const runtime::RigidBody *rigidBody = world.get_rigid_body_ptr(body);
  return (rigidBody != nullptr) ? math::length(rigidBody->velocity) : -1.0F;
}

/// Lowest world-space corner height of a rotated box.
float lowest_corner_y(const runtime::Transform &transform,
                      const math::Vec3 &halfExtents) {
  float lowest = 1.0e30F;
  for (int corner = 0; corner < 8; ++corner) {
    const math::Vec3 local((corner & 1) ? halfExtents.x : -halfExtents.x,
                           (corner & 2) ? halfExtents.y : -halfExtents.y,
                           (corner & 4) ? halfExtents.z : -halfExtents.z);
    const math::Vec3 world = math::add(
        math::rotate_vector(local, transform.rotation), transform.position);
    lowest = std::fmin(lowest, world.y);
  }
  return lowest;
}

/// Degrees between world up and the box axis nearest it: zero when the box
/// lies flat on any face.
float face_flat_degrees(const runtime::Transform &transform) {
  float best = 0.0F;
  const math::Vec3 axes[3] = {math::Vec3(1.0F, 0.0F, 0.0F),
                              math::Vec3(0.0F, 1.0F, 0.0F),
                              math::Vec3(0.0F, 0.0F, 1.0F)};
  for (const math::Vec3 &axis : axes) {
    best = std::fmax(
        best, std::fabs(math::rotate_vector(axis, transform.rotation).y));
  }
  return std::acos(std::fmin(1.0F, best)) * 57.2957795F;
}

math::Quat axis_angle(const math::Vec3 &axis, float radians) {
  const math::Vec3 unit = math::normalize(axis);
  const float s = std::sin(radians * 0.5F);
  return math::Quat(unit.x * s, unit.y * s, unit.z * s,
                    std::cos(radians * 0.5F));
}

// The rest suite's tolerances for a body on an analytic floor: the
// contact solver leaves under 5 mm of penetration at rest, and a body's
// centre within 1 cm of its resting height.
constexpr float kRestPenetration = 0.005F;
constexpr float kRestCentre = 0.01F;
constexpr float kRestSpeed = 0.05F;

void check_sphere_rests() {
  std::unique_ptr<runtime::World> world = make_world();
  if (world == nullptr) {
    g_tests.fail("a world is created");
    return;
  }
  add_floor(*world, true);
  runtime::Collider sphere{};
  sphere.shape = runtime::ColliderShape::Sphere;
  sphere.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  // Over a triangle edge: the shared diagonal of the quad at the origin.
  const runtime::Entity body =
      drop(*world, sphere, math::Vec3(0.25F, 3.0F, 0.75F), math::Quat());
  const runtime::Transform rest = settle(*world, body);
  g_tests.check(std::fabs(rest.position.y - 0.5F) <= kRestCentre,
                "a sphere comes to rest on the mesh floor");
  g_tests.check(speed(*world, body) <= kRestSpeed,
                "the sphere on the mesh is still");
  g_tests.check((std::fabs(rest.position.x - 0.25F) < 0.05F) &&
                    (std::fabs(rest.position.z - 0.75F) < 0.05F),
                "a sphere dropped on a flat mesh does not roll away");
}

void check_capsules_rest() {
  std::unique_ptr<runtime::World> world = make_world();
  if (world == nullptr) {
    g_tests.fail("a world is created");
    return;
  }
  add_floor(*world, true);
  runtime::Collider capsule{};
  capsule.shape = runtime::ColliderShape::Capsule;
  capsule.halfExtents = math::Vec3(0.3F, 0.5F, 0.3F);
  const runtime::Entity upright =
      drop(*world, capsule, math::Vec3(3.0F, 3.0F, 3.0F), math::Quat());
  const runtime::Entity lying =
      drop(*world, capsule, math::Vec3(-3.0F, 2.0F, -3.0F),
           axis_angle(math::Vec3(0.0F, 0.0F, 1.0F), 1.5707963F));
  const runtime::Transform uprightRest = settle(*world, upright);
  const runtime::Transform *lyingRest = world->get_transform_read_ptr(lying);
  g_tests.check(std::fabs(uprightRest.position.y - 0.8F) <= kRestCentre,
                "an upright capsule rests on its end cap");
  g_tests.check((lyingRest != nullptr) &&
                    (std::fabs(lyingRest->position.y - 0.3F) <= kRestCentre),
                "a capsule lying on the mesh rests on its side");
  if (lyingRest != nullptr) {
    const math::Vec3 axis =
        math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), lyingRest->rotation);
    g_tests.check(std::fabs(axis.y) < 0.02F,
                  "the lying capsule stays level across the triangles");
  }
  g_tests.check((speed(*world, upright) <= kRestSpeed) &&
                    (speed(*world, lying) <= kRestSpeed),
                "both capsules are still");
}

void check_tilted_box_settles_flat() {
  std::unique_ptr<runtime::World> world = make_world();
  if (world == nullptr) {
    g_tests.fail("a world is created");
    return;
  }
  add_floor(*world, true);
  runtime::Collider box{};
  box.shape = runtime::ColliderShape::AABB;
  box.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  const runtime::Entity body =
      drop(*world, box, math::Vec3(-1.5F, 2.0F, 2.5F),
           axis_angle(math::Vec3(1.0F, 0.0F, 1.0F), 0.35F));
  const runtime::Transform rest = settle(*world, body);
  g_tests.check(std::fabs(lowest_corner_y(rest, box.halfExtents)) <=
                    kRestPenetration,
                "a tilted box's lowest corner rests on the mesh");
  g_tests.check(face_flat_degrees(rest) < 2.0F,
                "the tilted box settles flat across the triangles");
  g_tests.check(speed(*world, body) <= kRestSpeed, "the box is still");
}

void check_mesh_on_moving_body_collides_with_nothing() {
  std::unique_ptr<runtime::World> world = make_world();
  if (world == nullptr) {
    g_tests.fail("a world is created");
    return;
  }
  const runtime::Entity floor = add_floor(*world, true);
  runtime::RigidBody floorBody{};
  floorBody.inverseMass = 1.0F;
  g_tests.check(world->add_rigid_body(floor, floorBody),
                "a body is put on the mesh");
  runtime::Collider sphere{};
  sphere.shape = runtime::ColliderShape::Sphere;
  sphere.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  const runtime::Entity body =
      drop(*world, sphere, math::Vec3(0.0F, 1.0F, 0.0F), math::Quat());
  const runtime::Transform rest = settle(*world, body);
  g_tests.check(rest.position.y < -1.0F,
                "a mesh on a moving body is not a floor");
  g_tests.check(world->physics_context().triMeshMovingPairsSkipped > 0U,
                "the skipped pairs are counted");
}

void check_missing_mesh_is_not_a_floor() {
  std::unique_ptr<runtime::World> world = make_world();
  if (world == nullptr) {
    g_tests.fail("a world is created");
    return;
  }
  add_floor(*world, false);
  runtime::Collider sphere{};
  sphere.shape = runtime::ColliderShape::Sphere;
  sphere.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  const runtime::Entity body =
      drop(*world, sphere, math::Vec3(0.0F, 1.0F, 0.0F), math::Quat());
  const runtime::Transform rest = settle(*world, body);
  g_tests.check(rest.position.y < -1.0F,
                "a TriMesh collider whose mesh is not installed is no floor");
}

/// A static TriMesh holding a floor of `n` x `n` quads and, when `wallX`
/// is finite, a wall: a vertical quad across the floor at x = `wallX`.
runtime::Entity add_mesh(runtime::World &world, float wallX) {
  std::vector<math::Vec3> vertices{};
  std::vector<std::uint32_t> indices{};
  const std::uint32_t n = 8U;
  const float half = 4.0F;
  for (std::uint32_t z = 0U; z <= n; ++z) {
    for (std::uint32_t x = 0U; x <= n; ++x) {
      vertices.emplace_back(static_cast<float>(x) - half, 0.0F,
                            static_cast<float>(z) - half);
    }
  }
  for (std::uint32_t z = 0U; z < n; ++z) {
    for (std::uint32_t x = 0U; x < n; ++x) {
      const std::uint32_t a = (z * (n + 1U)) + x;
      indices.insert(indices.end(),
                     {a, a + n + 1U, a + 1U, a + 1U, a + n + 1U, a + n + 2U});
    }
  }
  if (std::isfinite(wallX)) {
    const std::uint32_t w = static_cast<std::uint32_t>(vertices.size());
    vertices.emplace_back(wallX, 0.0F, -half);
    vertices.emplace_back(wallX, 3.0F, -half);
    vertices.emplace_back(wallX, 0.0F, half);
    vertices.emplace_back(wallX, 3.0F, half);
    indices.insert(indices.end(), {w, w + 1U, w + 2U, w + 2U, w + 1U, w + 3U});
  }
  physics::TriMeshRef mesh{};
  static_cast<void>(physics::build_tri_mesh(
      vertices.data(), vertices.size(), indices.data(), indices.size(), &mesh));
  const runtime::Entity entity =
      world.create_scene_object(runtime::Transform{});
  runtime::Collider collider{};
  collider.shape = runtime::ColliderShape::TriMesh;
  g_tests.check((entity != runtime::kInvalidEntity) &&
                    world.add_collider(entity, collider) &&
                    runtime::set_tri_mesh_data(world, entity, mesh),
                "the query mesh is created");
  return entity;
}

void check_queries_meet_triangles() {
  std::unique_ptr<runtime::World> world = make_world();
  if (world == nullptr) {
    g_tests.fail("a world is created");
    return;
  }
  const runtime::Entity mesh = add_mesh(*world, INFINITY);
  physics::PhysicsRaycastHit ray{};
  g_tests.check(runtime::raycast(*world, math::Vec3(0.3F, 5.0F, 0.4F),
                                 math::Vec3(0.0F, -1.0F, 0.0F), 10.0F, &ray) &&
                    (ray.entity == mesh) &&
                    (std::fabs(ray.distance - 5.0F) < 1.0e-5F) &&
                    (ray.normal.y > 0.99999F),
                "a ray down onto the mesh hits its floor, normal up");
  // The mesh's bounds are a flat box; a ray beside the floor but inside the
  // bounds' footprint height has nothing to hit.
  g_tests.check(!runtime::raycast(*world, math::Vec3(5.0F, 5.0F, 0.0F),
                                  math::Vec3(0.0F, -1.0F, 0.0F), 10.0F, &ray),
                "a ray beside the mesh misses it");

  physics::SweepHit sweep{};
  // Conservative advancement stops within its 1e-5 tolerance steps of the
  // surface, a few of which can accrue before the hit.
  g_tests.check(runtime::sweep_sphere(*world, math::Vec3(0.3F, 3.0F, 0.4F),
                                      0.5F, math::Vec3(0.0F, -1.0F, 0.0F),
                                      10.0F, &sweep) &&
                    (sweep.entityIndex == mesh.index) &&
                    (std::fabs(sweep.distance - 2.5F) < 1.0e-4F) &&
                    (sweep.normal.y > 0.99999F),
                "a sphere swept down stops on the floor, normal up");

  std::uint32_t found[4] = {};
  g_tests.check(runtime::overlap_sphere(*world, math::Vec3(0.2F, 0.4F, 0.1F),
                                        0.5F, found, 4U) == 1U,
                "a sphere dipping into the floor overlaps the mesh");
  g_tests.check(runtime::overlap_sphere(*world, math::Vec3(0.2F, 0.6F, 0.1F),
                                        0.5F, found, 4U) == 0U,
                "a sphere above the floor, inside the mesh's bounds, does not");
}

void check_character_walks_on_mesh() {
  std::unique_ptr<runtime::World> world = make_world();
  if (world == nullptr) {
    g_tests.fail("a world is created");
    return;
  }
  const runtime::Entity mesh = add_mesh(*world, 2.0F);
  constexpr float kRadius = 0.3F;
  constexpr float kSkin = 0.02F;
  physics::CharacterMoveSettings settings{};
  settings.skinWidth = kSkin;
  settings.wasGrounded = true;
  const auto capsule_at = [&](const math::Vec3 &feet) {
    return physics::CharacterCapsule{
        math::add(feet, math::Vec3(0.0F, kRadius, 0.0F)),
        math::add(feet, math::Vec3(0.0F, 1.8F - kRadius, 0.0F)), kRadius};
  };
  physics::CharacterMoveResult walk{};
  g_tests.check(physics::move_character(
                    *world, capsule_at(math::Vec3(-2.0F, kSkin, 0.3F)),
                    math::Vec3(1.0F, -0.1F, 0.0F), settings, &walk) &&
                    (std::fabs(walk.translation.x - 1.0F) < 1.0e-4F) &&
                    (std::fabs(walk.translation.y) < 1.0e-3F) &&
                    walk.grounded && (walk.ground == mesh),
                "a character walks across the mesh floor on its skin gap");

  physics::CharacterMoveResult blocked{};
  g_tests.check(physics::move_character(
                    *world, capsule_at(math::Vec3(0.0F, kSkin, 0.3F)),
                    math::Vec3(3.0F, 0.0F, 0.0F), settings, &blocked) &&
                    (blocked.translation.x < (2.0F - kRadius)) &&
                    (blocked.translation.x > (2.0F - kRadius - 2.0F * kSkin)) &&
                    ((blocked.flags & physics::kCharacterCollidedSides) != 0U),
                "a character stops at the mesh's wall, a skin gap short");
}

} // namespace

/// Runs the TriMesh collision suite.
int main() {
  check_sphere_rests();
  check_capsules_rest();
  check_tilted_box_settles_flat();
  check_mesh_on_moving_body_collides_with_nothing();
  check_missing_mesh_is_not_a_floor();
  check_queries_meet_triangles();
  check_character_walks_on_mesh();
  return g_tests.finish("tri mesh collision");
}
