#include <cmath>
#include <cstddef>

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

} // namespace

int main() {
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

  return 0;
}
