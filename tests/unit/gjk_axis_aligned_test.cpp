// Regression suite for issue #72: exactly axis-aligned convex pairs must
// produce a valid contact through the GJK/EPA path. Both shapes' support
// functions resolve a zero direction component to the same face corner, so
// every Minkowski sample collapses onto one line and the simplex never
// reaches a tetrahedron. The cases below pin the three reachable surfaces:
// the raw gjk_epa call (analytic expected depth), the compound-collider
// narrow phase that routes axis-aligned boxes through GJK, and box-vs-box
// CCD. Rotated variants are the controls that passed before the fix.

#include "engine/math/component_types.h"
#include "engine/math/quat.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/physics/ccd.h"
#include "engine/physics/collider.h"
#include "engine/physics/convex_hull.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

int g_passed = 0;
int g_failed = 0;

/// Records one assertion result.
void check(bool condition, const char *name) noexcept {
  if (condition) {
    ++g_passed;
  } else {
    ++g_failed;
    std::fprintf(stderr, "  FAIL: %s\n", name);
  }
}

// EPA converges to within kEpaTolerance (1e-4) of the exact boundary, so an
// analytically known depth is asserted at 1e-3 — tight enough that a wrong
// separating axis or a collapsed polytope cannot pass.
constexpr float kDepthTolerance = 1.0e-3F;

/// Support adapter for a world-space collider geometry (the same adapter the
/// affine narrow phase and CCD install).
engine::math::Vec3 support_geometry(const void *data,
                                    const engine::math::Vec3 & /*center*/,
                                    const engine::math::Vec3 &dir) noexcept {
  return engine::physics::collider_support_point(
      *static_cast<const engine::physics::ColliderWorldGeometry *>(data), dir);
}

/// Builds world geometry for an axis-aligned box collider at a position with
/// an optional rotation about +Z.
[[nodiscard]] bool
make_box_geometry(const engine::math::Vec3 &position, float halfExtent,
                  float zRotationRadians,
                  engine::physics::ColliderWorldGeometry *out) noexcept {
  engine::math::Collider collider{};
  collider.shape = engine::math::ColliderShape::AABB;
  collider.halfExtents =
      engine::math::Vec3(halfExtent, halfExtent, halfExtent);
  const engine::math::Quat rotation(
      0.0F, 0.0F, std::sin(zRotationRadians * 0.5F),
      std::cos(zRotationRadians * 0.5F));
  const engine::math::Mat4 world = engine::math::compose_trs(
      position, rotation, engine::math::Vec3(1.0F, 1.0F, 1.0F));
  return engine::physics::make_collider_world_geometry(collider, world, nullptr,
                                                       out);
}

/// The issue's exact reproduction: two overlapping unit boxes at (0,0,0) and
/// (0.6,0,0), both exactly axis-aligned. The Minkowski difference is the box
/// [-1.6,0.4]x[-1,1]x[-1,1], so the origin's nearest boundary is 0.4 along
/// +X and the penetration normal (A toward B) is +X.
void test_axis_aligned_box_overlap() noexcept {
  engine::physics::ColliderWorldGeometry a{};
  engine::physics::ColliderWorldGeometry b{};
  if (!make_box_geometry(engine::math::Vec3(0.0F, 0.0F, 0.0F), 0.5F, 0.0F,
                         &a) ||
      !make_box_geometry(engine::math::Vec3(0.6F, 0.0F, 0.0F), 0.5F, 0.0F,
                         &b)) {
    check(false, "axis-aligned box geometry build");
    return;
  }

  const engine::physics::GjkResult result = engine::physics::gjk_epa(
      &a, a.center, &support_geometry, &b, b.center, &support_geometry);
  check(result.intersecting, "axis-aligned overlapping boxes intersect");
  check(std::fabs(result.depth - 0.4F) <= kDepthTolerance,
        "axis-aligned box penetration depth is the analytic 0.4");
  check(std::fabs(result.normal.x - 1.0F) <= kDepthTolerance &&
            std::fabs(result.normal.y) <= kDepthTolerance &&
            std::fabs(result.normal.z) <= kDepthTolerance,
        "axis-aligned box penetration normal is +X");
}

/// Same overlap on the Y and Z axes, and along a diagonal offset: the
/// deepest axis must win in each case.
void test_axis_aligned_box_overlap_other_axes() noexcept {
  struct Case final {
    engine::math::Vec3 offset;
    engine::math::Vec3 expectedNormal;
    float expectedDepth;
    const char *name;
  };
  const Case cases[] = {
      {engine::math::Vec3(0.0F, 0.7F, 0.0F),
       engine::math::Vec3(0.0F, 1.0F, 0.0F), 0.3F, "Y-axis overlap"},
      {engine::math::Vec3(0.0F, 0.0F, -0.25F),
       engine::math::Vec3(0.0F, 0.0F, -1.0F), 0.75F, "-Z-axis overlap"},
      {engine::math::Vec3(0.9F, 0.2F, 0.0F),
       engine::math::Vec3(1.0F, 0.0F, 0.0F), 0.1F, "diagonal offset"},
  };

  for (const Case &testCase : cases) {
    engine::physics::ColliderWorldGeometry a{};
    engine::physics::ColliderWorldGeometry b{};
    if (!make_box_geometry(engine::math::Vec3(0.0F, 0.0F, 0.0F), 0.5F, 0.0F,
                           &a) ||
        !make_box_geometry(testCase.offset, 0.5F, 0.0F, &b)) {
      check(false, testCase.name);
      continue;
    }
    const engine::physics::GjkResult result = engine::physics::gjk_epa(
        &a, a.center, &support_geometry, &b, b.center, &support_geometry);
    const bool depthOk =
        std::fabs(result.depth - testCase.expectedDepth) <= kDepthTolerance;
    const bool normalOk =
        std::fabs(result.normal.x - testCase.expectedNormal.x) <=
            kDepthTolerance &&
        std::fabs(result.normal.y - testCase.expectedNormal.y) <=
            kDepthTolerance &&
        std::fabs(result.normal.z - testCase.expectedNormal.z) <=
            kDepthTolerance;
    check(result.intersecting && depthOk && normalOk, testCase.name);
  }
}

/// Boundary cases: exactly touching faces carry no penetration to resolve,
/// and a separated axis-aligned pair must still report no intersection (the
/// degenerate-simplex handler must not manufacture contacts).
void test_axis_aligned_boundaries() noexcept {
  engine::physics::ColliderWorldGeometry a{};
  engine::physics::ColliderWorldGeometry touching{};
  engine::physics::ColliderWorldGeometry separated{};
  engine::physics::ColliderWorldGeometry coincident{};
  if (!make_box_geometry(engine::math::Vec3(0.0F, 0.0F, 0.0F), 0.5F, 0.0F,
                         &a) ||
      !make_box_geometry(engine::math::Vec3(1.0F, 0.0F, 0.0F), 0.5F, 0.0F,
                         &touching) ||
      !make_box_geometry(engine::math::Vec3(1.25F, 0.0F, 0.0F), 0.5F, 0.0F,
                         &separated) ||
      !make_box_geometry(engine::math::Vec3(0.0F, 0.0F, 0.0F), 0.5F, 0.0F,
                         &coincident)) {
    check(false, "axis-aligned boundary geometry build");
    return;
  }

  const engine::physics::GjkResult touchResult =
      engine::physics::gjk_epa(&a, a.center, &support_geometry, &touching,
                               touching.center, &support_geometry);
  check(touchResult.depth <= kDepthTolerance,
        "exactly touching axis-aligned faces carry no penetration");

  const engine::physics::GjkResult separatedResult =
      engine::physics::gjk_epa(&a, a.center, &support_geometry, &separated,
                               separated.center, &support_geometry);
  check(!separatedResult.intersecting,
        "separated axis-aligned boxes do not intersect");

  const engine::physics::GjkResult coincidentResult =
      engine::physics::gjk_epa(&a, a.center, &support_geometry, &coincident,
                               coincident.center, &support_geometry);
  check(coincidentResult.intersecting &&
            std::isfinite(coincidentResult.depth) &&
            (coincidentResult.depth <= 1.0F + kDepthTolerance),
        "coincident axis-aligned boxes report a bounded depth");
}

/// Control from the issue: a small rotation off the axes already produced a
/// contact before the fix and must keep producing one.
void test_rotated_box_overlap_control() noexcept {
  engine::physics::ColliderWorldGeometry a{};
  engine::physics::ColliderWorldGeometry b{};
  if (!make_box_geometry(engine::math::Vec3(0.0F, 0.0F, 0.0F), 0.5F, 0.0F,
                         &a) ||
      !make_box_geometry(engine::math::Vec3(0.6F, 0.0F, 0.0F), 0.5F, 0.2F,
                         &b)) {
    check(false, "rotated box geometry build");
    return;
  }
  const engine::physics::GjkResult result = engine::physics::gjk_epa(
      &a, a.center, &support_geometry, &b, b.center, &support_geometry);
  check(result.intersecting && std::isfinite(result.depth) &&
            (result.depth > 0.4F) && (result.depth < 1.0F),
        "rotated box control still intersects");
}

/// Deterministic 32-bit LCG so the sweep below is bit-identical on every run
/// and platform.
class Lcg final {
public:
  /// Returns the next value in [0,1).
  [[nodiscard]] float next() noexcept {
    m_state = (m_state * 1664525U) + 1013904223U;
    return static_cast<float>((m_state >> 8U) & 0xFFFFU) / 65536.0F;
  }

private:
  std::uint32_t m_state = 12345U;
};

/// Builds a unit quaternion from an axis and angle.
[[nodiscard]] engine::math::Quat axis_angle(const engine::math::Vec3 &axis,
                                            float angle) noexcept {
  const float length = engine::math::length(axis);
  const float scale = std::sin(angle * 0.5F) / length;
  return engine::math::Quat(axis.x * scale, axis.y * scale, axis.z * scale,
                            std::cos(angle * 0.5F));
}

/// Builds world geometry for an arbitrarily rotated collider.
[[nodiscard]] bool
make_rotated_geometry(const engine::math::Vec3 &position,
                      const engine::math::Quat &rotation,
                      engine::math::ColliderShape shape,
                      engine::physics::ColliderWorldGeometry *out) noexcept {
  engine::math::Collider collider{};
  collider.shape = shape;
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  return engine::physics::make_collider_world_geometry(
      collider,
      engine::math::compose_trs(position, rotation,
                                engine::math::Vec3(1.0F, 1.0F, 1.0F)),
      nullptr, out);
}

/// Issue #72 finding 2: EPA must never report a penetration deeper than the
/// geometry allows. A spinning box against a spinning box or ball drives the
/// polytope through its degenerate configurations; before the fix this sweep
/// produced depths of 1e30 (the closest-face search's sentinel, returned
/// verbatim once every face had been retired) and hundreds of results past
/// the geometric bound. Two half-0.5 boxes cannot penetrate deeper than the
/// sum of their bounding radii, sqrt(3)/2 each.
void test_epa_depth_never_exceeds_geometry() noexcept {
  constexpr int kSamples = 40000;
  constexpr float kMaxPlausibleDepth = 1.7320509F;
  Lcg random;
  int violations = 0;
  int nonFinite = 0;
  int badNormals = 0;

  for (int i = 0; i < kSamples; ++i) {
    const engine::math::Vec3 axis((random.next() * 2.0F) - 0.999F,
                                  (random.next() * 2.0F) - 1.0F,
                                  (random.next() * 2.0F) - 1.0F);
    const engine::math::Quat rotationA = axis_angle(axis, random.next() * 6.283F);
    const engine::math::Vec3 offset(((random.next() * 2.0F) - 1.0F) * 1.2F,
                                    ((random.next() * 2.0F) - 1.0F) * 1.2F,
                                    ((random.next() * 2.0F) - 1.0F) * 1.2F);
    const engine::math::Quat rotationB = axis_angle(axis, random.next() * 6.283F);

    engine::physics::ColliderWorldGeometry a{};
    engine::physics::ColliderWorldGeometry b{};
    if (!make_rotated_geometry(engine::math::Vec3(0.0F, 0.0F, 0.0F), rotationA,
                               engine::math::ColliderShape::AABB, &a) ||
        !make_rotated_geometry(offset, rotationB,
                               ((i % 2) == 0)
                                   ? engine::math::ColliderShape::Sphere
                                   : engine::math::ColliderShape::AABB,
                               &b)) {
      continue;
    }

    const engine::physics::GjkResult result = engine::physics::gjk_epa(
        &a, a.center, &support_geometry, &b, b.center, &support_geometry);
    if (!result.intersecting) {
      continue;
    }
    if (!std::isfinite(result.depth) || !std::isfinite(result.normal.x) ||
        !std::isfinite(result.normal.y) || !std::isfinite(result.normal.z)) {
      ++nonFinite;
      continue;
    }
    if (result.depth > kMaxPlausibleDepth) {
      ++violations;
    }
    const float normalLength = engine::math::length(result.normal);
    if ((result.depth > 1.0e-6F) && (std::fabs(normalLength - 1.0F) > 0.01F)) {
      ++badNormals;
    }
  }

  check(nonFinite == 0, "EPA never reports a non-finite penetration");
  check(violations == 0, "EPA never reports a depth past the geometric bound");
  check(badNormals == 0, "EPA penetration normals stay unit length");
}

std::size_t g_pairCount = 0U;

/// Collision dispatch sink for the production-path compound case.
void record_collision_pairs(const engine::runtime::Entity * /*pairs*/,
                            std::size_t pairCount) noexcept {
  g_pairCount = pairCount;
}

/// Creates a World parked in its component-mutation phase.
[[nodiscard]] std::unique_ptr<engine::runtime::World> make_world() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world != nullptr) {
    world->end_frame_phase();
  }
  return world;
}

/// Runs one full production frame of transform composition plus collision
/// resolution and dispatch.
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

/// Production path: a compound child box (collider under a rigid-body root)
/// overlapping a static box, both exactly axis-aligned. Compound membership
/// forces the affine GJK narrow phase, so before the fix this pair produced
/// no contact at all.
void test_compound_axis_aligned_box_contact() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    check(false, "compound world allocation");
    return;
  }
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  engine::runtime::Transform staticTransform{};
  const engine::runtime::Entity staticBox =
      world->create_scene_object(staticTransform);

  engine::runtime::Transform rootTransform{};
  rootTransform.position = engine::math::Vec3(0.6F, 0.0F, 0.0F);
  const engine::runtime::Entity root =
      world->create_scene_object(rootTransform);

  engine::runtime::Transform childTransform{};
  childTransform.parentId = world->persistent_id(root);
  const engine::runtime::Entity child =
      world->create_scene_object(childTransform);
  if (staticBox == engine::runtime::kInvalidEntity ||
      root == engine::runtime::kInvalidEntity ||
      child == engine::runtime::kInvalidEntity) {
    check(false, "compound entity creation");
    return;
  }

  engine::runtime::Collider box{};
  box.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::RigidBody dynamicBody{};
  dynamicBody.inverseMass = 1.0F;
  if (!world->add_collider(staticBox, box) ||
      !world->add_collider(child, box) ||
      !world->add_rigid_body(root, dynamicBody)) {
    check(false, "compound component setup");
    return;
  }

  g_pairCount = 0U;
  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);
  const bool resolved = resolve_frame(*world);
  check(resolved && (g_pairCount == 1U),
        "compound axis-aligned box pair records a contact");

  engine::runtime::Transform moved{};
  const bool hasTransform = world->get_transform(root, &moved);
  check(hasTransform && (moved.position.x > rootTransform.position.x),
        "compound axis-aligned contact separates the rigid-body root along +X");
}

/// Production path: box-vs-box CCD along an exactly axis-aligned sweep. The
/// bullet and wall share their Y and Z half extents, so the tied support
/// components cancel exactly and every Minkowski sample lands on the X axis
/// — the collapse the issue reports. The bullet's leading face reaches the
/// wall's near face at 0.976 of the step (4.88 m of the 5 m travel), and the
/// bilateral advancement's kTolerance (1e-4 over a 5 m step, i.e. 2e-5 of
/// normalized time) bounds the error.
void test_axis_aligned_box_ccd_impact() noexcept {
  std::unique_ptr<engine::runtime::World> world = make_world();
  if (world == nullptr) {
    check(false, "CCD world allocation");
    return;
  }
  engine::runtime::set_gravity(*world, 0.0F, 0.0F, 0.0F);

  const engine::runtime::Entity bullet = world->create_entity();
  engine::runtime::Transform bulletTransform{};
  world->add_transform(bullet, bulletTransform);
  engine::runtime::Collider bulletCollider{};
  bulletCollider.halfExtents = engine::math::Vec3(0.1F, 2.0F, 2.0F);
  world->add_collider(bullet, bulletCollider);
  engine::runtime::RigidBody bulletBody{};
  bulletBody.inverseMass = 1.0F;
  bulletBody.velocity = engine::math::Vec3(300.0F, 0.0F, 0.0F);
  world->add_rigid_body(bullet, bulletBody);

  const engine::runtime::Entity wall = world->create_entity();
  engine::runtime::Transform wallTransform{};
  wallTransform.position = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  world->add_transform(wall, wallTransform);
  engine::runtime::Collider wallCollider{};
  wallCollider.halfExtents = engine::math::Vec3(0.02F, 2.0F, 2.0F);
  world->add_collider(wall, wallCollider);

  const engine::physics::CcdSweepResult hit =
      engine::physics::bilateral_advance_ccd(*world, bullet, bulletBody,
                                             bulletCollider, bulletTransform,
                                             1.0F / 60.0F);
  check(hit.hit, "axis-aligned box CCD reports an impact");
  check(hit.hitEntityIndex == wall.index, "axis-aligned box CCD names the wall");
  check(std::fabs(hit.timeOfImpact - 0.976F) <= 1.0e-3F,
        "axis-aligned box CCD time of impact");
}

} // namespace

/// Runs this executable or test program.
int main() {
  test_axis_aligned_box_overlap();
  test_axis_aligned_box_overlap_other_axes();
  test_axis_aligned_boundaries();
  test_rotated_box_overlap_control();
  test_epa_depth_never_exceeds_geometry();
  test_compound_axis_aligned_box_contact();
  test_axis_aligned_box_ccd_impact();

  std::printf("gjk_axis_aligned_test: %d passed, %d failed\n", g_passed,
              g_failed);
  return (g_failed > 0) ? 1 : 0;
}
