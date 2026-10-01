// Verifies math test behavior for the Engine test suite.

#include <cmath>
#include <cstddef>
#include <limits>

#include "engine/math/aabb.h"
#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/ray.h"
#include "engine/math/sphere.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"

namespace {

bool nearly_equal(float a, float b, float epsilon) {
  return std::fabs(a - b) <= epsilon;
}

bool nearly_equal_vec3(const engine::math::Vec3 &a, const engine::math::Vec3 &b,
                       float epsilon) {
  return nearly_equal(a.x, b.x, epsilon) && nearly_equal(a.y, b.y, epsilon) &&
         nearly_equal(a.z, b.z, epsilon);
}

bool nearly_equal_quat(const engine::math::Quat &a, const engine::math::Quat &b,
                       float epsilon) {
  const float cosTheta = std::fabs(engine::math::dot(a, b));
  return cosTheta >= (1.0F - epsilon);
}

bool check_identity(const engine::math::Mat4 &value, float epsilon) {
  const engine::math::Mat4 identity = engine::math::identity();
  for (std::size_t col = 0U; col < 4U; ++col) {
    const engine::math::Vec4 &lhs = value.columns[col];
    const engine::math::Vec4 &rhs = identity.columns[col];

    if (!nearly_equal(lhs.x, rhs.x, epsilon) ||
        !nearly_equal(lhs.y, rhs.y, epsilon) ||
        !nearly_equal(lhs.z, rhs.z, epsilon) ||
        !nearly_equal(lhs.w, rhs.w, epsilon)) {
      return false;
    }
  }

  return true;
}

bool nearly_equal_angle(float a, float b, float epsilon) {
  const float twoPi = 6.2831853072F;
  float delta = std::fmod(a - b, twoPi);
  if (delta > 3.1415926536F) {
    delta -= twoPi;
  } else if (delta < -3.1415926536F) {
    delta += twoPi;
  }

  return std::fabs(delta) <= epsilon;
}

/// A direction rotated by a unit quaternion keeps its authored magnitude:
/// a quarter turn about +Z carries (2, 0, 0) onto (0, 2, 0). Tolerance
/// 1e-4 absolute on O(1) components, the float rounding of the Rodrigues
/// form, so a scaled or mirrored result cannot pass.
bool check_rotate_vector_preserves_magnitude() {
  constexpr float kHalfPi = 1.57079632679F;
  const engine::math::Quat rotation = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 0.0F, 1.0F), kHalfPi);
  const engine::math::Vec3 direction =
      engine::math::rotate_vector(engine::math::Vec3(2.0F, 0.0F, 0.0F),
                                  rotation);
  return nearly_equal(direction.x, 0.0F, 1.0e-4F) &&
         nearly_equal(direction.y, 2.0F, 1.0e-4F) &&
         nearly_equal(direction.z, 0.0F, 1.0e-4F);
}

/// Conservative transformed half extents include world rotation,
/// non-uniform scale, and negative scale signs: a quarter turn about +Z
/// with scale (-2, 3, 4) maps local half extents (1, 2, 3) onto world
/// (6, 2, 12), the |x| of the mirrored axis included. Same 1e-4 absolute
/// tolerance as the rotation check, for the same reason.
bool check_transform_aabb_half_extents() {
  constexpr float kHalfPi = 1.57079632679F;
  const engine::math::Mat4 world = engine::math::compose_trs(
      engine::math::Vec3(10.0F, 20.0F, 30.0F),
      engine::math::from_axis_angle(engine::math::Vec3(0.0F, 0.0F, 1.0F),
                                    kHalfPi),
      engine::math::Vec3(-2.0F, 3.0F, 4.0F));
  const engine::math::Vec3 half = engine::math::transform_aabb_half_extents(
      world, engine::math::Vec3(1.0F, 2.0F, 3.0F));
  return nearly_equal(half.x, 6.0F, 1.0e-4F) &&
         nearly_equal(half.y, 2.0F, 1.0e-4F) &&
         nearly_equal(half.z, 12.0F, 1.0e-4F);
}

/// transform_point applies the whole affine matrix (w = 1), and is bit
/// for bit the multiply the physics and render-prep copies it replaced
/// ran; transform_vector drops the translation (w = 0); transform_aabb
/// moves the centre as a point and widens the half extents. Under
/// translate (10, 20, 30), a quarter turn about z and scale (-2, 3, 4),
/// x = 1 goes to (0, -2, 0) before translation. The quarter turn's cosine
/// is -4.4e-8 in float, hence the 1e-4 bound on derived values.
bool check_point_and_vector_transforms() {
  constexpr float kHalfPi = 1.57079632679F;
  const engine::math::Mat4 world = engine::math::compose_trs(
      engine::math::Vec3(10.0F, 20.0F, 30.0F),
      engine::math::from_axis_angle(engine::math::Vec3(0.0F, 0.0F, 1.0F),
                                    kHalfPi),
      engine::math::Vec3(-2.0F, 3.0F, 4.0F));
  const engine::math::Vec3 unitX(1.0F, 0.0F, 0.0F);
  const engine::math::Vec3 point = engine::math::transform_point(world, unitX);
  const engine::math::Vec4 direct =
      engine::math::mul(world, engine::math::Vec4(1.0F, 0.0F, 0.0F, 1.0F));
  const engine::math::Vec3 vector =
      engine::math::transform_vector(world, unitX);
  engine::math::Vec3 center{};
  engine::math::Vec3 half{};
  engine::math::transform_aabb(
      world, unitX, engine::math::Vec3(1.0F, 2.0F, 3.0F), &center, &half);
  return (point.x == direct.x) && (point.y == direct.y) &&
         (point.z == direct.z) && nearly_equal(point.x, 10.0F, 1.0e-4F) &&
         nearly_equal(point.y, 18.0F, 1.0e-4F) &&
         nearly_equal(point.z, 30.0F, 1.0e-4F) &&
         nearly_equal(vector.x, 0.0F, 1.0e-4F) &&
         nearly_equal(vector.y, -2.0F, 1.0e-4F) &&
         nearly_equal(vector.z, 0.0F, 1.0e-4F) && (center.x == point.x) &&
         (center.y == point.y) && (center.z == point.z) &&
         nearly_equal(half.x, 6.0F, 1.0e-4F) &&
         nearly_equal(half.y, 2.0F, 1.0e-4F) &&
         nearly_equal(half.z, 12.0F, 1.0e-4F);
}

/// look_rotation points -Z along the forward it is given, keeps +Y on the
/// up hint's side and the right axis level, and refuses a zero forward or
/// one parallel to up. The bound is a few dozen ulps of 1: the result
/// passes through one normalize, two cross products, Shepperd's sqrt and
/// a final normalize, each within a few ulps.
bool check_look_rotation() {
  using engine::math::Quat;
  using engine::math::Vec3;
  constexpr float kBound = 1.0e-5F;
  const Vec3 up(0.0F, 1.0F, 0.0F);
  const Vec3 forwards[] = {Vec3(0.0F, 0.0F, -1.0F), Vec3(0.0F, 0.0F, 1.0F),
                           Vec3(1.0F, 0.0F, 0.0F),  Vec3(-3.0F, 0.0F, 0.0F),
                           Vec3(1.0F, -1.0F, 1.0F), Vec3(0.2F, 0.97F, 0.1F),
                           Vec3(0.0F, -5.0F, 4.0F)};
  for (const Vec3 &forward : forwards) {
    Quat q{};
    if (!engine::math::look_rotation(forward, up, &q)) {
      return false;
    }
    const Vec3 lookDir =
        engine::math::rotate_vector(Vec3(0.0F, 0.0F, -1.0F), q);
    const Vec3 rotatedUp =
        engine::math::rotate_vector(Vec3(0.0F, 1.0F, 0.0F), q);
    const Vec3 rotatedRight =
        engine::math::rotate_vector(Vec3(1.0F, 0.0F, 0.0F), q);
    if (!nearly_equal_vec3(lookDir, engine::math::normalize(forward), kBound) ||
        !nearly_equal(engine::math::dot(q, q), 1.0F, kBound) ||
        !(rotatedUp.y > 0.0F) ||
        !nearly_equal(engine::math::dot(rotatedUp, lookDir), 0.0F, kBound) ||
        !nearly_equal(rotatedRight.y, 0.0F, kBound)) {
      return false;
    }
  }
  // Facing -Z with +Y up is no rotation at all.
  Quat identity(0.5F, 0.5F, 0.5F, 0.5F);
  if (!engine::math::look_rotation(Vec3(0.0F, 0.0F, -2.0F), up, &identity) ||
      !nearly_equal_quat(identity, Quat(), kBound)) {
    return false;
  }
  const Quat sentinel(0.5F, 0.5F, 0.5F, 0.5F);
  Quat untouched = sentinel;
  return !engine::math::look_rotation(Vec3(0.0F, 0.0F, 0.0F), up, &untouched) &&
         !engine::math::look_rotation(Vec3(0.0F, 3.0F, 0.0F), up, &untouched) &&
         !engine::math::look_rotation(Vec3(0.0F, -1.0F, 0.0F), up,
                                      &untouched) &&
         (untouched.x == sentinel.x) && (untouched.w == sentinel.w);
}

/// A ray beside or through a small sphere far down its path. The textbook
/// discriminant b^2 - 4ac subtracts two values near 4 D^2, so at 100 m the
/// miss margin of a 5 cm sphere was rounding noise: a ray 6 cm off its
/// centre reported a hit, and at 10 km any ray within 2 m did. Returns 0,
/// or the failing row's code.
int check_far_ray_sphere() noexcept {
  const float distances[3] = {100.0F, 1000.0F, 10000.0F};
  constexpr float kRadius = 0.05F;
  int code = 900;
  for (const float distance : distances) {
    engine::math::Ray ray{};
    ray.origin = engine::math::Vec3(0.0F, 0.0F, 0.0F);
    ray.direction = engine::math::Vec3(0.0F, 0.0F, -1.0F);
    engine::math::Sphere sphere{};
    sphere.radius = kRadius;

    // Passing 1 cm outside the surface: a miss at every range.
    sphere.center = engine::math::Vec3(0.06F, 0.0F, -distance);
    if (engine::math::ray_intersects_sphere(ray, sphere, nullptr)) {
      return code;
    }
    // 1 cm inside the silhouette: still a hit.
    sphere.center = engine::math::Vec3(0.04F, 0.0F, -distance);
    if (!engine::math::ray_intersects_sphere(ray, sphere, nullptr)) {
      return code + 1;
    }
    // Through the centre: the near surface at D - r. The root is c / q
    // with c = D^2 - r^2 and q = D + r, each rounded once, so the result
    // is within a few roundings of D; four ulps of D bounds it.
    sphere.center = engine::math::Vec3(0.0F, 0.0F, -distance);
    float t = -1.0F;
    const float tolerance =
        4.0F * distance * std::numeric_limits<float>::epsilon();
    if (!engine::math::ray_intersects_sphere(ray, sphere, &t) ||
        (std::fabs(t - (distance - kRadius)) > tolerance)) {
      return code + 2;
    }
    code += 10;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
/// decompose_trs takes only TRS: the sheared basis a rotated child of a
/// (2,1,1) parent has, a singular basis and a non-finite one are refused;
/// what it returns has a unit rotation; a mirror folds into scale.z; and
/// basis_rotation still names the rotation of the sheared basis.
int check_decompose_refuses_non_trs() noexcept {
  using namespace engine::math;
  const Mat4 parent =
      compose_trs(Vec3(0.0F, 0.0F, 0.0F), Quat(), Vec3(2.0F, 1.0F, 1.0F));
  Mat4 inverseParent{};
  if (!inverse(parent, &inverseParent)) {
    return 240;
  }
  const Quat turn = from_axis_angle(Vec3(0.0F, 0.0F, 1.0F), 0.785398163F);
  const Mat4 sheared = mul(mul(inverseParent, to_mat4(turn)), parent);
  Vec3 t{};
  Quat r{};
  Vec3 s{};
  if (decompose_trs(sheared, &t, &r, &s)) {
    return 241;
  }
  Mat4 singular = identity();
  singular.columns[1] = singular.columns[0];
  if (decompose_trs(singular, &t, &r, &s)) {
    return 242;
  }
  Mat4 flat = identity();
  flat.columns[2] = Vec4(0.0F, 0.0F, 0.0F, 0.0F);
  Mat4 notFinite = identity();
  notFinite.columns[3].x = std::numeric_limits<float>::infinity();
  if (decompose_trs(flat, &t, &r, &s) || decompose_trs(notFinite, &t, &r, &s)) {
    return 243;
  }
  // A TRS matrix decomposes, with a rotation of unit length; one ulp of
  // float around 1 bounds the length's error.
  const Quat q = normalize(Quat(0.1F, 0.7F, -0.3F, 0.6F));
  if (!decompose_trs(
          compose_trs(Vec3(1.0F, 2.0F, 3.0F), q, Vec3(0.5F, 3.0F, 1.5F)), &t,
          &r, &s) ||
      (std::fabs(std::sqrt(dot(r, r)) - 1.0F) > 1.2e-7F)) {
    return 244;
  }
  // A mirror folds into a negative scale.z.
  if (!decompose_trs(compose_trs(Vec3(), Quat(), Vec3(1.0F, 1.0F, -2.0F)), &t,
                     &r, &s) ||
      (s.z != -2.0F) || (s.x != 1.0F)) {
    return 245;
  }
  // The sheared basis still has a rotation: its X axis turned 45 degrees.
  Quat basis{};
  if (!basis_rotation(mul(to_mat4(turn), parent), &basis) ||
      !nearly_equal_quat(basis, turn, 1.0e-6F)) {
    return 246;
  }
  return 0;
}

int main() {
  if (const int farRay = check_far_ray_sphere(); farRay != 0) {
    return farRay;
  }
  if (const int trs = check_decompose_refuses_non_trs(); trs != 0) {
    return trs;
  }

  const engine::math::Vec2 v2a(1.0F, 2.0F);
  const engine::math::Vec2 v2b(3.0F, 4.0F);
  const engine::math::Vec2 v2c = engine::math::add(v2a, v2b);

  if ((v2c.x != 4.0F) || (v2c.y != 6.0F)) {
    return 1;
  }

  if (!nearly_equal(engine::math::length(v2a), std::sqrt(5.0F), 1.0e-4F)) {
    return 2;
  }

  const engine::math::Vec3 v3x(1.0F, 0.0F, 0.0F);
  const engine::math::Vec3 v3y(0.0F, 1.0F, 0.0F);
  const engine::math::Vec3 v3z = engine::math::cross(v3x, v3y);

  if ((v3z.x != 0.0F) || (v3z.y != 0.0F) || (v3z.z != 1.0F)) {
    return 3;
  }

  const engine::math::Mat4 id = engine::math::identity();
  const engine::math::Vec4 v4(1.0F, 2.0F, 3.0F, 1.0F);
  const engine::math::Vec4 v4out = engine::math::mul(id, v4);

  if (!nearly_equal(v4out.x, v4.x, 1.0e-4F) ||
      !nearly_equal(v4out.y, v4.y, 1.0e-4F) ||
      !nearly_equal(v4out.z, v4.z, 1.0e-4F) ||
      !nearly_equal(v4out.w, v4.w, 1.0e-4F)) {
    return 4;
  }

  const engine::math::Quat rot = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 1.0F, 0.0F), 0.75F);
  const engine::math::Mat4 trs =
      engine::math::compose_trs(engine::math::Vec3(1.0F, 2.0F, 3.0F), rot,
                                engine::math::Vec3(2.0F, 2.0F, 2.0F));

  engine::math::Mat4 inv{};
  if (!engine::math::inverse(trs, &inv)) {
    return 5;
  }

  const engine::math::Mat4 product = engine::math::mul(trs, inv);
  if (!check_identity(product, 1.0e-3F)) {
    return 6;
  }

  const engine::math::Quat q0 = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 1.0F, 0.0F), 0.25F);
  const engine::math::Quat q1 =
      engine::math::from_axis_angle(engine::math::Vec3(0.0F, 1.0F, 0.0F), 1.0F);
  const engine::math::Quat slerpStart = engine::math::slerp(q0, q1, 0.0F);
  const engine::math::Quat slerpEnd = engine::math::slerp(q0, q1, 1.0F);

  if (!nearly_equal_quat(slerpStart, q0, 1.0e-4F)) {
    return 7;
  }

  if (!nearly_equal_quat(slerpEnd, q1, 1.0e-4F)) {
    return 8;
  }

  const engine::math::Mat4 view =
      engine::math::look_at(engine::math::Vec3(0.0F, 0.0F, 0.0F),
                            engine::math::Vec3(0.0F, 0.0F, -1.0F),
                            engine::math::Vec3(0.0F, 1.0F, 0.0F));
  const engine::math::Vec3 right(view.columns[0].x, view.columns[0].y,
                                 view.columns[0].z);
  const engine::math::Vec3 up(view.columns[1].x, view.columns[1].y,
                              view.columns[1].z);
  const engine::math::Vec3 forward(view.columns[2].x, view.columns[2].y,
                                   view.columns[2].z);

  if (std::fabs(engine::math::dot(right, up)) > 1.0e-3F) {
    return 9;
  }

  if (std::fabs(engine::math::dot(right, forward)) > 1.0e-3F) {
    return 10;
  }

  if (std::fabs(engine::math::dot(up, forward)) > 1.0e-3F) {
    return 11;
  }

  if (!nearly_equal(engine::math::length(right), 1.0F, 1.0e-3F) ||
      !nearly_equal(engine::math::length(up), 1.0F, 1.0e-3F) ||
      !nearly_equal(engine::math::length(forward), 1.0F, 1.0e-3F)) {
    return 12;
  }

  const float nearZ = 0.1F;
  const float farZ = 100.0F;
  const engine::math::Mat4 proj =
      engine::math::perspective(1.0F, 1.0F, nearZ, farZ);
  const engine::math::Vec4 nearPoint(0.0F, 0.0F, -nearZ, 1.0F);
  const engine::math::Vec4 farPoint(0.0F, 0.0F, -farZ, 1.0F);
  const engine::math::Vec4 nearClip = engine::math::mul(proj, nearPoint);
  const engine::math::Vec4 farClip = engine::math::mul(proj, farPoint);

  if (nearClip.w == 0.0F || farClip.w == 0.0F) {
    return 13;
  }

  const float nearNdc = nearClip.z / nearClip.w;
  const float farNdc = farClip.z / farClip.w;
  if (!nearly_equal(nearNdc, -1.0F, 1.0e-3F) ||
      !nearly_equal(farNdc, 1.0F, 1.0e-3F)) {
    return 14;
  }

  // ortho(): a point on each boundary plane maps to the matching NDC face
  // and w stays exactly 1 (#221 pins the previously untested helper).
  const engine::math::Mat4 orthoProj =
      engine::math::ortho(-2.0F, 2.0F, -1.0F, 1.0F, nearZ, farZ);
  const engine::math::Vec4 orthoCorner(2.0F, -1.0F, -nearZ, 1.0F);
  const engine::math::Vec4 orthoFar(0.0F, 0.0F, -farZ, 1.0F);
  const engine::math::Vec4 cornerClip =
      engine::math::mul(orthoProj, orthoCorner);
  const engine::math::Vec4 orthoFarClip =
      engine::math::mul(orthoProj, orthoFar);
  if ((cornerClip.w != 1.0F) || (orthoFarClip.w != 1.0F)) {
    return 15;
  }
  if (!nearly_equal(cornerClip.x, 1.0F, 1.0e-3F) ||
      !nearly_equal(cornerClip.y, -1.0F, 1.0e-3F) ||
      !nearly_equal(cornerClip.z, -1.0F, 1.0e-3F) ||
      !nearly_equal(orthoFarClip.z, 1.0F, 1.0e-3F)) {
    return 16;
  }

  engine::math::Vec3 outTranslation{};
  engine::math::Quat outRotation{};
  engine::math::Vec3 outScale{};
  if (!engine::math::decompose_trs(trs, &outTranslation, &outRotation,
                                   &outScale)) {
    return 15;
  }

  if (!nearly_equal_vec3(outTranslation, engine::math::Vec3(1.0F, 2.0F, 3.0F),
                         1.0e-3F)) {
    return 16;
  }

  if (!nearly_equal_vec3(outScale, engine::math::Vec3(2.0F, 2.0F, 2.0F),
                         1.0e-3F)) {
    return 17;
  }

  if (!nearly_equal_quat(outRotation, rot, 1.0e-3F)) {
    return 18;
  }

  const engine::math::Mat4 rotMat = engine::math::to_mat4(q1);
  const engine::math::Quat fromRot = engine::math::from_mat4(rotMat);
  if (!nearly_equal_quat(fromRot, q1, 1.0e-3F)) {
    return 19;
  }

  float outPitch = 0.0F;
  float outYaw = 0.0F;
  float outRoll = 0.0F;

  const float pitchOnly = 0.5F;
  const engine::math::Quat qPitch =
      engine::math::from_euler(pitchOnly, 0.0F, 0.0F);
  if (!engine::math::to_euler(qPitch, &outPitch, &outYaw, &outRoll)) {
    return 20;
  }
  if (!nearly_equal_angle(outPitch, pitchOnly, 1.0e-4F) ||
      !nearly_equal_angle(outYaw, 0.0F, 1.0e-4F) ||
      !nearly_equal_angle(outRoll, 0.0F, 1.0e-4F)) {
    return 21;
  }

  const float yawOnly = 0.5F;
  const engine::math::Quat qYaw = engine::math::from_euler(0.0F, yawOnly, 0.0F);
  if (!engine::math::to_euler(qYaw, &outPitch, &outYaw, &outRoll)) {
    return 22;
  }
  if (!nearly_equal_angle(outPitch, 0.0F, 1.0e-4F) ||
      !nearly_equal_angle(outYaw, yawOnly, 1.0e-4F) ||
      !nearly_equal_angle(outRoll, 0.0F, 1.0e-4F)) {
    return 23;
  }

  const float rollOnly = 0.5F;
  const engine::math::Quat qRoll =
      engine::math::from_euler(0.0F, 0.0F, rollOnly);
  if (!engine::math::to_euler(qRoll, &outPitch, &outYaw, &outRoll)) {
    return 24;
  }
  if (!nearly_equal_angle(outPitch, 0.0F, 1.0e-4F) ||
      !nearly_equal_angle(outYaw, 0.0F, 1.0e-4F) ||
      !nearly_equal_angle(outRoll, rollOnly, 1.0e-4F)) {
    return 25;
  }

  // Combined pitch+yaw+roll round trip (issue #156 regression): the prior
  // to_euler formula was only ever exercised with one nonzero axis at a
  // time (the three checks above), which happened to leave a wrong-
  // composition-order bug undetected -- it inverted an aircraft-attitude
  // (ZYX) formula against from_euler's actual Ry*Rx*Rz (YXZ) composition,
  // so any two-or-more-axis rotation reconstructed a materially different
  // orientation on round trip (dot product as low as ~0.65 for the case
  // below, red on the pre-fix formula). Each case here recovers both the
  // exact input angles and an equivalent quaternion (away from the
  // pitch = +-90 deg pole, where the yaw/roll split is documented as
  // ambiguous but the reconstructed rotation must still match).
  {
    const float kCases[][3] = {
        {0.5235988F, 0.7853982F, 1.0471976F},    // 30, 45, 60 deg
        {-0.3490659F, 1.2217305F, -2.3561945F},  // -20, 70, -135 deg
        {0.1745329F, -0.1745329F, 0.1745329F},   // 10, -10, 10 deg
    };
    for (const auto &c : kCases) {
      const engine::math::Quat original =
          engine::math::from_euler(c[0], c[1], c[2]);
      float p = 0.0F;
      float y = 0.0F;
      float r = 0.0F;
      if (!engine::math::to_euler(original, &p, &y, &r)) {
        return 37;
      }
      if (!nearly_equal_angle(p, c[0], 1.0e-3F) ||
          !nearly_equal_angle(y, c[1], 1.0e-3F) ||
          !nearly_equal_angle(r, c[2], 1.0e-3F)) {
        return 38;
      }
      const engine::math::Quat roundTrip = engine::math::from_euler(p, y, r);
      if (!nearly_equal_quat(original, roundTrip, 1.0e-4F)) {
        return 39;
      }
    }
  }

  const engine::math::Quat invalidAxis = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 0.0F, 0.0F), 0.75F);
  const engine::math::Quat identityQ{};
  if (!nearly_equal_quat(invalidAxis, identityQ, 1.0e-4F)) {
    return 26;
  }

  engine::math::Sphere unitSphere{};
  unitSphere.center = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  unitSphere.radius = 1.0F;

  engine::math::Ray degenerateRay{};
  degenerateRay.origin = engine::math::Vec3(0.0F, 0.0F, -2.0F);
  degenerateRay.direction = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  if (engine::math::ray_intersects_sphere(degenerateRay, unitSphere, nullptr)) {
    return 27;
  }

  // Slab intersection per axis: axis-aligned rays along y and z hit the
  // unit box exactly one unit out (these axes previously went through
  // member-pointer indexing; the per-axis accessor must agree).
  engine::math::AABB unitBox{};
  unitBox.min = engine::math::Vec3(-1.0F, -1.0F, -1.0F);
  unitBox.max = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  const engine::math::Vec3 axisDirections[3] = {
      engine::math::Vec3(1.0F, 0.0F, 0.0F),
      engine::math::Vec3(0.0F, 1.0F, 0.0F),
      engine::math::Vec3(0.0F, 0.0F, 1.0F),
  };
  for (int axis = 0; axis < 3; ++axis) {
    engine::math::Ray axisRay{};
    axisRay.origin = engine::math::Vec3(
        (axis == 0) ? -2.0F : 0.0F, (axis == 1) ? -2.0F : 0.0F,
        (axis == 2) ? -2.0F : 0.0F);
    axisRay.direction = axisDirections[axis];
    float t = -1.0F;
    if (!engine::math::ray_intersects_aabb(axisRay, unitBox, &t) ||
        (t != 1.0F)) {
      return 28;
    }
    axisRay.direction = engine::math::Vec3(
        -axisDirections[axis].x, -axisDirections[axis].y,
        -axisDirections[axis].z);
    if (engine::math::ray_intersects_aabb(axisRay, unitBox, nullptr)) {
      return 29;
    }
  }

  // Zero and signed-zero directions: parallel axes pass only when the
  // origin lies inside that slab.
  engine::math::Ray parallelRay{};
  parallelRay.origin = engine::math::Vec3(0.0F, 0.0F, -2.0F);
  parallelRay.direction = engine::math::Vec3(0.0F, -0.0F, 1.0F);
  float parallelT = -1.0F;
  if (!engine::math::ray_intersects_aabb(parallelRay, unitBox, &parallelT) ||
      (parallelT != 1.0F)) {
    return 30;
  }
  parallelRay.origin = engine::math::Vec3(2.0F, 0.0F, -2.0F);
  if (engine::math::ray_intersects_aabb(parallelRay, unitBox, nullptr)) {
    return 31;
  }
  parallelRay.origin = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  parallelRay.direction = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  float insideT = -1.0F;
  if (!engine::math::ray_intersects_aabb(parallelRay, unitBox, &insideT) ||
      (insideT != 0.0F)) {
    return 32;
  }

  // Boundary origins: starting exactly on a face reports a hit at t = 0
  // whether the ray points inward or away along that axis.
  engine::math::Ray faceRay{};
  faceRay.origin = engine::math::Vec3(-1.0F, 0.0F, 0.0F);
  faceRay.direction = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  float faceT = -1.0F;
  if (!engine::math::ray_intersects_aabb(faceRay, unitBox, &faceT) ||
      (faceT != 0.0F)) {
    return 33;
  }
  faceRay.origin = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  if (!engine::math::ray_intersects_aabb(faceRay, unitBox, &faceT) ||
      (faceT != 0.0F)) {
    return 34;
  }

  // An infinite direction component collapses that axis to t = 0; the
  // other axes still gate the result.
  engine::math::Ray infiniteRay{};
  infiniteRay.origin = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  infiniteRay.direction = engine::math::Vec3(
      std::numeric_limits<float>::infinity(), 0.0F, 0.0F);
  float infiniteT = -1.0F;
  if (!engine::math::ray_intersects_aabb(infiniteRay, unitBox, &infiniteT) ||
      (infiniteT != 0.0F)) {
    return 35;
  }
  infiniteRay.origin = engine::math::Vec3(0.0F, 5.0F, 0.0F);
  if (engine::math::ray_intersects_aabb(infiniteRay, unitBox, nullptr)) {
    return 36;
  }

  // Non-unit tiny directions (audit #336): the slab test must treat only an
  // exactly-zero component as parallel, so a legitimate direction of any
  // representable magnitude still hits with t scaling as 1/magnitude. The
  // 1e-4 relative tolerance covers the float rounding of the reciprocal and
  // slab arithmetic; the expectation itself is exact algebra (t = 1/s for a
  // unit-distance gap).
  engine::math::AABB offsetBox{};
  offsetBox.min = engine::math::Vec3(0.0F, -1.0F, -1.0F);
  offsetBox.max = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  const float directionScales[3] = {1.0e-3F, 1.0e-6F, 1.0e-9F};
  for (const float scale : directionScales) {
    engine::math::Ray tinyRay{};
    tinyRay.origin = engine::math::Vec3(-1.0F, 0.0F, 0.0F);
    tinyRay.direction = engine::math::Vec3(scale, 0.0F, 0.0F);
    float tinyT = -1.0F;
    if (!engine::math::ray_intersects_aabb(tinyRay, offsetBox, &tinyT)) {
      return 40;
    }
    const float expectedT = 1.0F / scale;
    if (std::fabs(tinyT - expectedT) > (1.0e-4F * expectedT)) {
      return 41;
    }

    engine::math::Ray tinySphereRay{};
    tinySphereRay.origin = engine::math::Vec3(0.0F, 0.0F, -2.0F);
    tinySphereRay.direction = engine::math::Vec3(0.0F, 0.0F, scale);
    float sphereT = -1.0F;
    if (!engine::math::ray_intersects_sphere(tinySphereRay, unitSphere,
                                             &sphereT)) {
      return 42;
    }
    if (std::fabs(sphereT - expectedT) > (1.0e-4F * expectedT)) {
      return 43;
    }
  }

  // The sphere test's degeneracy boundary is the representable limit: a
  // direction whose squared length underflows to exactly zero is refused
  // (indistinguishable from a zero direction in float), not one that merely
  // fails a magnitude cutoff.
  engine::math::Ray underflowRay{};
  underflowRay.origin = engine::math::Vec3(0.0F, 0.0F, -2.0F);
  underflowRay.direction = engine::math::Vec3(0.0F, 0.0F, 1.0e-25F);
  if (engine::math::ray_intersects_sphere(underflowRay, unitSphere, nullptr)) {
    return 44;
  }

  // Inverse conditioning (issue #389): singularity is judged by the
  // usability of the computed inverse, not an absolute pivot cutoff, so a
  // rotated TRS with a finite nonzero small axis scale stays invertible
  // down to (and past) the old absolute 1e-6 pivot threshold. Tolerance
  // justification: identity entries are O(1), so the absolute bound
  // doubles as a relative one; the Gauss-Jordan residual measured at most
  // ~1e-6 (about 8 float epsilons) across these cases, and 1e-5 leaves
  // 10x headroom without ever accepting a wrong inverse.
  {
    const engine::math::Quat smallRot = engine::math::from_axis_angle(
        engine::math::Vec3(0.3F, 0.8F, 0.52F), 0.9F);
    const float smallScales[] = {1.0e-3F, 1.0e-6F, 1.0e-7F};
    int code = 45;
    for (const float smallScale : smallScales) {
      const engine::math::Mat4 nonuniform = engine::math::compose_trs(
          engine::math::Vec3(1.0F, 2.0F, 3.0F), smallRot,
          engine::math::Vec3(smallScale, 1.0F, 2.0F));
      engine::math::Mat4 smallInv{};
      if (!engine::math::inverse(nonuniform, &smallInv)) {
        return code;
      }
      if (!check_identity(engine::math::mul(nonuniform, smallInv), 1.0e-5F)) {
        return code + 1;
      }

      const engine::math::Mat4 uniform = engine::math::compose_trs(
          engine::math::Vec3(-4.0F, 0.5F, 9.0F), smallRot,
          engine::math::Vec3(smallScale, smallScale, smallScale));
      if (!engine::math::inverse(uniform, &smallInv)) {
        return code + 2;
      }
      if (!check_identity(engine::math::mul(uniform, smallInv), 1.0e-5F)) {
        return code + 3;
      }
      code += 4;
    }
  }

  // True degeneracy and non-finite input stay rejected, and *out stays
  // untouched on refusal.
  {
    const engine::math::Quat smallRot = engine::math::from_axis_angle(
        engine::math::Vec3(0.0F, 1.0F, 0.0F), 0.4F);
    const engine::math::Mat4 zeroScale = engine::math::compose_trs(
        engine::math::Vec3(1.0F, 1.0F, 1.0F), smallRot,
        engine::math::Vec3(0.0F, 1.0F, 1.0F));
    engine::math::Mat4 sentinel = engine::math::identity();
    sentinel.columns[3].x = 123.0F;
    const engine::math::Mat4 sentinelCopy = sentinel;
    if (engine::math::inverse(zeroScale, &sentinel)) {
      return 57;
    }

    engine::math::Mat4 nanMatrix = engine::math::identity();
    nanMatrix.columns[1].y = std::numeric_limits<float>::quiet_NaN();
    if (engine::math::inverse(nanMatrix, &sentinel)) {
      return 58;
    }

    engine::math::Mat4 infMatrix = engine::math::identity();
    infMatrix.columns[2].z = std::numeric_limits<float>::infinity();
    if (engine::math::inverse(infMatrix, &sentinel)) {
      return 59;
    }

    for (std::size_t col = 0U; col < 4U; ++col) {
      if (!nearly_equal(sentinel.columns[col].x, sentinelCopy.columns[col].x,
                        0.0F) ||
          !nearly_equal(sentinel.columns[col].y, sentinelCopy.columns[col].y,
                        0.0F) ||
          !nearly_equal(sentinel.columns[col].z, sentinelCopy.columns[col].z,
                        0.0F) ||
          !nearly_equal(sentinel.columns[col].w, sentinelCopy.columns[col].w,
                        0.0F)) {
        return 60;
      }
    }
  }

  if (!check_rotate_vector_preserves_magnitude()) {
    return 61;
  }
  if (!check_transform_aabb_half_extents()) {
    return 62;
  }
  if (!check_point_and_vector_transforms()) {
    return 63;
  }
  if (!check_look_rotation()) {
    return 64;
  }

  return 0;
}
