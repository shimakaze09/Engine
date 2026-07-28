// Verifies exact affine transforms, support points, and bounds for colliders.

#include "engine/math/component_types.h"
#include "engine/math/mat4.h"
#include "engine/math/vec4.h"
#include "engine/physics/collider.h"

#include <cstdio>
#include <limits>

namespace {

[[nodiscard]] bool equal(const engine::math::Vec3 &left,
                         const engine::math::Vec3 &right) noexcept {
  return left.x == right.x && left.y == right.y && left.z == right.z;
}

[[nodiscard]] engine::math::Mat4
affine_matrix(const engine::math::Vec3 &column0,
              const engine::math::Vec3 &column1,
              const engine::math::Vec3 &column2,
              const engine::math::Vec3 &translation) noexcept {
  return engine::math::Mat4(
      engine::math::Vec4(column0.x, column0.y, column0.z, 0.0F),
      engine::math::Vec4(column1.x, column1.y, column1.z, 0.0F),
      engine::math::Vec4(column2.x, column2.y, column2.z, 0.0F),
      engine::math::Vec4(translation.x, translation.y, translation.z, 1.0F));
}

[[nodiscard]] bool check(bool condition, const char *message) noexcept {
  if (!condition) {
    std::fprintf(stderr, "collider geometry test failed: %s\n", message);
  }
  return condition;
}

[[nodiscard]] bool test_box_transform() noexcept {
  engine::math::Collider collider{};
  collider.shape = engine::math::ColliderShape::AABB;
  collider.localPosition = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  collider.localRotation = engine::math::Quat(0.0F, 0.0F, 1.0F, 0.0F);
  collider.halfExtents = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  const engine::math::Mat4 entityWorld =
      affine_matrix(engine::math::Vec3(0.0F, 2.0F, 0.0F),
                    engine::math::Vec3(-3.0F, 0.0F, 0.0F),
                    engine::math::Vec3(0.0F, 0.0F, -4.0F),
                    engine::math::Vec3(10.0F, 20.0F, 30.0F));

  engine::physics::ColliderWorldGeometry geometry{};
  if (!check(engine::physics::make_collider_world_geometry(
                 collider, entityWorld, nullptr, &geometry),
             "box geometry builds")) {
    return false;
  }
  return check(equal(geometry.center, engine::math::Vec3(4.0F, 22.0F, 18.0F)),
               "box local position follows entity transform") &&
         check(equal(geometry.worldAabb.min,
                     engine::math::Vec3(-2.0F, 20.0F, 6.0F)),
               "box minimum includes rotation and signed scale") &&
         check(equal(geometry.worldAabb.max,
                     engine::math::Vec3(10.0F, 24.0F, 30.0F)),
               "box maximum includes rotation and signed scale") &&
         check(engine::physics::collider_support_point(
                   geometry, engine::math::Vec3(1.0F, 0.0F, 0.0F))
                       .x == 10.0F,
               "box support reaches positive world x") &&
         check(geometry.localToWorld.columns[0].y == -2.0F &&
                   geometry.localToWorld.columns[1].x == 3.0F,
               "box local rotation composes with entity rotation");
}

[[nodiscard]] bool test_sphere_ellipsoid() noexcept {
  engine::math::Collider collider{};
  collider.shape = engine::math::ColliderShape::Sphere;
  collider.halfExtents = engine::math::Vec3(1.0F, 9.0F, 9.0F);
  const engine::math::Mat4 entityWorld =
      affine_matrix(engine::math::Vec3(2.0F, 0.0F, 0.0F),
                    engine::math::Vec3(0.0F, -3.0F, 0.0F),
                    engine::math::Vec3(0.0F, 0.0F, 4.0F),
                    engine::math::Vec3(5.0F, 6.0F, 7.0F));

  engine::physics::ColliderWorldGeometry geometry{};
  return check(engine::physics::make_collider_world_geometry(
                   collider, entityWorld, nullptr, &geometry),
               "sphere geometry builds") &&
         check(equal(geometry.worldAabb.min,
                     engine::math::Vec3(3.0F, 3.0F, 3.0F)),
               "sphere becomes an ellipsoid under nonuniform signed scale") &&
         check(equal(geometry.worldAabb.max,
                     engine::math::Vec3(7.0F, 9.0F, 11.0F)),
               "ellipsoid maximum is exact") &&
         check(equal(engine::physics::collider_support_point(
                         geometry, engine::math::Vec3(0.0F, 1.0F, 0.0F)),
                     engine::math::Vec3(5.0F, 9.0F, 7.0F)),
               "sphere support handles negative scale");
}

[[nodiscard]] bool test_rotated_capsule() noexcept {
  engine::math::Collider collider{};
  collider.shape = engine::math::ColliderShape::Capsule;
  collider.halfExtents = engine::math::Vec3(1.0F, 2.0F, 1.0F);
  const engine::math::Mat4 entityWorld =
      affine_matrix(engine::math::Vec3(0.0F, 1.0F, 0.0F),
                    engine::math::Vec3(-1.0F, 0.0F, 0.0F),
                    engine::math::Vec3(0.0F, 0.0F, 1.0F),
                    engine::math::Vec3(10.0F, 20.0F, 30.0F));

  engine::physics::ColliderWorldGeometry geometry{};
  return check(engine::physics::make_collider_world_geometry(
                   collider, entityWorld, nullptr, &geometry),
               "capsule geometry builds") &&
         check(equal(geometry.worldAabb.min,
                     engine::math::Vec3(7.0F, 19.0F, 29.0F)),
               "rotated capsule minimum is exact") &&
         check(equal(geometry.worldAabb.max,
                     engine::math::Vec3(13.0F, 21.0F, 31.0F)),
               "rotated capsule maximum is exact") &&
         check(equal(engine::physics::collider_support_point(
                         geometry, engine::math::Vec3(1.0F, 0.0F, 0.0F)),
                     engine::math::Vec3(13.0F, 20.0F, 30.0F)),
               "capsule support follows its rotated axis");
}

[[nodiscard]] bool test_signed_scaled_hull() noexcept {
  engine::physics::ConvexHullData hull{};
  const engine::math::Vec3 minimum(-1.0F, -2.0F, -3.0F);
  const engine::math::Vec3 maximum(2.0F, 1.0F, 4.0F);
  hull.vertices[0] = engine::math::Vec3(minimum.x, minimum.y, minimum.z);
  hull.vertices[1] = engine::math::Vec3(maximum.x, minimum.y, minimum.z);
  hull.vertices[2] = engine::math::Vec3(minimum.x, maximum.y, minimum.z);
  hull.vertices[3] = engine::math::Vec3(maximum.x, maximum.y, minimum.z);
  hull.vertices[4] = engine::math::Vec3(minimum.x, minimum.y, maximum.z);
  hull.vertices[5] = engine::math::Vec3(maximum.x, minimum.y, maximum.z);
  hull.vertices[6] = engine::math::Vec3(minimum.x, maximum.y, maximum.z);
  hull.vertices[7] = engine::math::Vec3(maximum.x, maximum.y, maximum.z);
  hull.vertexCount = 8U;

  engine::math::Collider collider{};
  collider.shape = engine::math::ColliderShape::ConvexHull;
  const engine::math::Mat4 entityWorld =
      affine_matrix(engine::math::Vec3(-2.0F, 0.0F, 0.0F),
                    engine::math::Vec3(0.0F, 3.0F, 0.0F),
                    engine::math::Vec3(0.0F, 0.0F, 4.0F),
                    engine::math::Vec3(5.0F, 6.0F, 7.0F));

  engine::physics::ColliderWorldGeometry geometry{};
  return check(engine::physics::make_collider_world_geometry(
                   collider, entityWorld, &hull, &geometry),
               "hull geometry builds") &&
         check(equal(geometry.worldAabb.min,
                     engine::math::Vec3(1.0F, 0.0F, -5.0F)),
               "hull signed-scale minimum uses transformed vertices") &&
         check(equal(geometry.worldAabb.max,
                     engine::math::Vec3(7.0F, 9.0F, 23.0F)),
               "hull signed-scale maximum uses transformed vertices") &&
         check(engine::physics::collider_support_point(
                   geometry, engine::math::Vec3(1.0F, 0.0F, 0.0F))
                       .x == 7.0F,
               "hull support reverses across negative scale");
}

[[nodiscard]] bool test_invalid_transforms() noexcept {
  engine::math::Collider collider{};
  engine::physics::ColliderWorldGeometry geometry{};
  geometry.center = engine::math::Vec3(9.0F, 8.0F, 7.0F);
  const engine::math::Mat4 singular =
      affine_matrix(engine::math::Vec3(0.0F, 0.0F, 0.0F),
                    engine::math::Vec3(0.0F, 1.0F, 0.0F),
                    engine::math::Vec3(0.0F, 0.0F, 1.0F),
                    engine::math::Vec3(0.0F, 0.0F, 0.0F));
  if (!check(!engine::physics::make_collider_world_geometry(collider, singular,
                                                            nullptr, &geometry),
             "singular transform is rejected") ||
      !check(equal(geometry.center, engine::math::Vec3(9.0F, 8.0F, 7.0F)),
             "failed build leaves output unchanged")) {
    return false;
  }

  engine::math::Mat4 nonfinite{};
  nonfinite.columns[0].x = std::numeric_limits<float>::quiet_NaN();
  return check(!engine::physics::make_collider_world_geometry(
                   collider, nonfinite, nullptr, &geometry),
               "nonfinite transform is rejected");
}

} // namespace

int main() {
  const engine::math::Collider defaults{};
  if (!check(
          equal(defaults.localPosition, engine::math::Vec3(0.0F, 0.0F, 0.0F)),
          "collider local position defaults to zero") ||
      !check(defaults.localRotation.x == 0.0F &&
                 defaults.localRotation.y == 0.0F &&
                 defaults.localRotation.z == 0.0F &&
                 defaults.localRotation.w == 1.0F,
             "collider local rotation defaults to identity") ||
      !test_box_transform() || !test_sphere_ellipsoid() ||
      !test_rotated_capsule() || !test_signed_scaled_hull() ||
      !test_invalid_transforms()) {
    return 1;
  }
  return 0;
}
