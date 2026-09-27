// Verifies the one view-frustum primitive in engine/math/frustum.h:
// extracted planes are unit and inward and sit where the projection puts
// them under both clip depth conventions; the box, swept box and sphere
// tests exclude only what is wholly outside a plane; a sub-rectangle
// projection's frustum is the part of the view behind that rectangle; and
// unprojection refuses a point with no finite image. The camera sits at
// the origin looking down -Z with a 90 degree field of view, so the side
// planes are x = +-z and y = +-z, the near plane is z = -1 and the far
// plane z = -100.

#include "engine/math/frustum.h"

#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"

#include "../test_harness.h"

#include <cmath>

namespace {

using engine::math::Frustum;
using engine::math::Mat4;
using engine::math::Vec3;

constexpr float kHalfPi = 1.57079632679F;
constexpr float kNear = 1.0F;
constexpr float kFar = 100.0F;

// Plane coefficients come from sums of float matrix entries; the near and
// side planes land within a few ulps, so distances of order 1 hold to
// 1e-5. The far plane is the difference of the w and z rows, whose
// entries are ~1 and nearly cancel: (f+n)/(n-f) carries a relative
// rounding error near 6e-8, magnified by f, so distances there hold to
// 1e-4 absolute.
constexpr float kPlaneTol = 1.0e-5F;
constexpr float kFarPlaneTol = 1.0e-4F;

Mat4 projection(bool zeroOne) noexcept {
  return zeroOne
             ? engine::math::perspective_zero_one(kHalfPi, 1.0F, kNear, kFar)
             : engine::math::perspective(kHalfPi, 1.0F, kNear, kFar);
}

/// The frustum of the test camera: the view is the identity.
Frustum camera_frustum(bool zeroOne) noexcept {
  return engine::math::frustum_from_view_projection(projection(zeroOne),
                                                    zeroOne);
}

bool near_abs(float actual, float expected, float tol) noexcept {
  return std::fabs(actual - expected) <= tol;
}

void check_planes(engine::tests::TestContext &t, bool zeroOne) noexcept {
  const Frustum frustum = camera_frustum(zeroOne);
  bool unit = true;
  for (const engine::math::Plane &plane : frustum.planes) {
    unit = unit && near_abs(engine::math::length(plane.normal), 1.0F, 1.0e-6F);
  }
  t.check(unit, zeroOne ? "[0,1]: every plane normal is unit"
                        : "[-1,1]: every plane normal is unit");

  // Signed distances of the axis point at depth 10: inside every plane,
  // 10 sin 45 from each side plane, 9 from the near and 90 from the far.
  const Vec3 axis(0.0F, 0.0F, -10.0F);
  const float side = 10.0F * std::sqrt(0.5F);
  bool sides = true;
  for (int i = 0; i < 4; ++i) {
    sides =
        sides && near_abs(engine::math::plane_distance(frustum.planes[i], axis),
                          side, kPlaneTol * 10.0F);
  }
  t.check(sides, zeroOne ? "[0,1]: the side planes face inward at 45 degrees"
                         : "[-1,1]: the side planes face inward at 45 degrees");
  t.check(near_abs(engine::math::plane_distance(frustum.planes[4], axis), 9.0F,
                   kPlaneTol * 10.0F),
          zeroOne ? "[0,1]: the near plane is at the near distance"
                  : "[-1,1]: the near plane is at the near distance");
  t.check(near_abs(engine::math::plane_distance(frustum.planes[5], axis), 90.0F,
                   kFarPlaneTol * 10.0F),
          zeroOne ? "[0,1]: the far plane is at the far distance"
                  : "[-1,1]: the far plane is at the far distance");
}

/// The near plane follows the projection's convention. A [0, 1] matrix
/// read with GL's rule puts the near plane where its clip z is -w, at
/// fn/(2f - n) = 0.5025 here, about half the near distance: light culling
/// did that on every device, so a light wholly between the camera and the
/// near plane still reached the tiles.
void check_near_plane_convention(engine::tests::TestContext &t) noexcept {
  const Mat4 zeroOneProjection = projection(true);
  const Frustum right =
      engine::math::frustum_from_view_projection(zeroOneProjection, true);
  const Frustum glRule =
      engine::math::frustum_from_view_projection(zeroOneProjection, false);
  const Vec3 beforeNear(0.0F, 0.0F, -0.75F);
  t.check(engine::math::frustum_excludes_sphere(right, beforeNear, 0.1F),
          "a sphere between the camera and the near plane is excluded");
  t.check(!engine::math::frustum_excludes_sphere(glRule, beforeNear, 0.1F),
          "GL's rule on a [0, 1] matrix would keep it");
  t.check(near_abs(engine::math::plane_distance(glRule.planes[4],
                                                Vec3(0.0F, 0.0F, 0.0F)),
                   -100.0F / 199.0F, kPlaneTol),
          "GL's rule on a [0, 1] matrix puts the near plane at fn/(2f-n)");
}

void check_box_tests(engine::tests::TestContext &t) noexcept {
  const Frustum frustum = camera_frustum(false);
  const Vec3 half(1.0F, 1.0F, 1.0F);
  t.check(!engine::math::frustum_excludes_box(frustum, Vec3(0.0F, 0.0F, -10.0F),
                                              half),
          "a box inside is kept");
  t.check(engine::math::frustum_excludes_box(frustum, Vec3(0.0F, 0.0F, -102.0F),
                                             half),
          "a box wholly beyond the far plane is excluded");
  t.check(
      engine::math::frustum_excludes_box(frustum, Vec3(0.0F, 0.0F, 5.0F), half),
      "a box behind the camera is excluded");
  t.check(!engine::math::frustum_excludes_box(frustum,
                                              Vec3(10.0F, 0.0F, -10.0F), half),
          "a box straddling a side plane is kept");
  // Past the far-right edge: x in [101, 110], z in [-120, -99]. Inside
  // needs z >= -100 and x <= -z <= 100, so no point of it is, yet it is
  // wholly behind neither plane: the test is conservative, so it is kept
  // (a draw, never a missing one).
  t.check(!engine::math::frustum_excludes_box(
              frustum, Vec3(105.5F, 0.0F, -109.5F), Vec3(4.5F, 1.0F, 10.5F)),
          "a box outside only past an edge is kept");

  // Swept toward the view, a box behind the camera can cast into it.
  const Vec3 behind(0.0F, 0.0F, 5.0F);
  t.check(!engine::math::frustum_excludes_swept_box(frustum, behind, half,
                                                    Vec3(0.0F, 0.0F, -10.0F)),
          "a box swept into the view is kept");
  t.check(engine::math::frustum_excludes_swept_box(frustum, behind, half,
                                                   Vec3(0.0F, 0.0F, 10.0F)),
          "a box swept away from the view is excluded");
  t.check(engine::math::frustum_excludes_swept_box(frustum, behind, half,
                                                   Vec3(0.0F, 0.0F, 0.0F)) ==
              engine::math::frustum_excludes_box(frustum, behind, half),
          "a zero sweep is the plain box test");
}

void check_sphere_tests(engine::tests::TestContext &t) noexcept {
  const Frustum frustum = camera_frustum(false);
  // At depth 10 the right plane is x = 10; the sphere's centre is 2 sin 45
  // = 1.414 outside it.
  const Vec3 outside(12.0F, 0.0F, -10.0F);
  t.check(engine::math::frustum_excludes_sphere(frustum, outside, 1.0F),
          "a sphere wholly outside a side plane is excluded");
  t.check(!engine::math::frustum_excludes_sphere(frustum, outside, 1.5F),
          "a sphere reaching across the plane is kept");
  t.check(!engine::math::frustum_excludes_sphere(
              frustum, Vec3(0.0F, 0.0F, -1.0F), 0.25F),
          "a sphere straddling the near plane is kept");
}

/// A sub-rectangle's frustum is the part of the view behind it. The whole
/// NDC square gives back the projection exactly; the right half's left
/// plane is the view's centre line.
void check_sub_rect(engine::tests::TestContext &t) noexcept {
  const Mat4 proj = projection(false);
  const Mat4 whole =
      engine::math::sub_rect_projection(proj, -1.0F, -1.0F, 1.0F, 1.0F);
  bool same = true;
  for (int c = 0; c < 4; ++c) {
    same = same && (whole.columns[c].x == proj.columns[c].x) &&
           (whole.columns[c].y == proj.columns[c].y) &&
           (whole.columns[c].z == proj.columns[c].z) &&
           (whole.columns[c].w == proj.columns[c].w);
  }
  t.check(same, "the whole square gives back the projection");

  const Frustum rightHalf = engine::math::frustum_from_view_projection(
      engine::math::sub_rect_projection(proj, 0.0F, -1.0F, 1.0F, 1.0F), false);
  // NDC x = 0.5 at depth 10 is world x = 5, inside the right half; x = -5
  // is in the left half, 5 from the centre line.
  t.check(!engine::math::frustum_excludes_sphere(
              rightHalf, Vec3(5.0F, 0.0F, -10.0F), 1.0F),
          "a sphere behind the rectangle is kept");
  t.check(engine::math::frustum_excludes_sphere(
              rightHalf, Vec3(-5.0F, 0.0F, -10.0F), 1.0F),
          "a sphere beside the rectangle is excluded");
  t.check(near_abs(engine::math::plane_distance(rightHalf.planes[0],
                                                Vec3(-5.0F, 0.0F, -10.0F)),
                   -5.0F, kPlaneTol * 10.0F),
          "the right half's left plane is the centre line");
}

/// A point or a corner with no finite image is refused, not divided by
/// zero; the corners come back near quad first.
void check_unprojection(engine::tests::TestContext &t) noexcept {
  Mat4 zero = engine::math::identity();
  zero.columns[0] = engine::math::Vec4();
  zero.columns[1] = engine::math::Vec4();
  zero.columns[2] = engine::math::Vec4();
  zero.columns[3] = engine::math::Vec4();
  Vec3 point{};
  Vec3 corners[8]{};
  t.check(!engine::math::unproject_ndc(zero, Vec3(0.0F, 0.0F, 0.0F), &point),
          "a point with w = 0 is refused");
  t.check(!engine::math::frustum_corners(zero, false, corners),
          "corners with w = 0 are refused");

  Mat4 inverse{};
  t.check(engine::math::inverse(projection(true), &inverse) &&
              engine::math::frustum_corners(inverse, true, corners),
          "the [0,1] frustum's corners unproject");
  // The near quad at z = -1 spans +-1, bottom-left first; the far quad
  // follows at z = -100.
  t.check(near_abs(corners[0].x, -1.0F, kPlaneTol) &&
              near_abs(corners[0].y, -1.0F, kPlaneTol) &&
              near_abs(corners[0].z, -kNear, kPlaneTol) &&
              near_abs(corners[2].x, 1.0F, kPlaneTol) &&
              near_abs(corners[2].y, 1.0F, kPlaneTol),
          "the near quad comes first, bottom-left to top-right");
  t.check(near_abs(corners[4].z, -kFar, kFar * 1.0e-3F) &&
              (corners[4].x < 0.0F) && (corners[6].x > 0.0F),
          "the far quad follows");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  check_planes(t, false);
  check_planes(t, true);
  check_near_plane_convention(t);
  check_box_tests(t);
  check_sphere_tests(t);
  check_sub_rect(t);
  check_unprojection(t);
  return t.finish("math_frustum");
}
