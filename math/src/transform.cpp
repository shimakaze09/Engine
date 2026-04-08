#include "engine/math/transform.h"

#include <cassert>
#include <cmath>

#include "engine/math/vec3.h"

namespace engine::math {

Mat4 compose_trs(const Vec3 &translation, const Quat &rotation,
                 const Vec3 &scale) noexcept {
  Mat4 result = to_mat4(rotation);
  result.columns[0] = mul(result.columns[0], scale.x);
  result.columns[1] = mul(result.columns[1], scale.y);
  result.columns[2] = mul(result.columns[2], scale.z);
  result.columns[3] = Vec4(translation.x, translation.y, translation.z, 1.0F);
  return result;
}

bool decompose_trs(const Mat4 &value, Vec3 *outTranslation, Quat *outRotation,
                   Vec3 *outScale) noexcept {
  if ((outTranslation == nullptr) || (outRotation == nullptr) ||
      (outScale == nullptr)) {
    return false;
  }

  const Vec3 translation(value.columns[3].x, value.columns[3].y,
                         value.columns[3].z);
  const Vec3 basisX(value.columns[0].x, value.columns[0].y, value.columns[0].z);
  const Vec3 basisY(value.columns[1].x, value.columns[1].y, value.columns[1].z);
  const Vec3 basisZ(value.columns[2].x, value.columns[2].y, value.columns[2].z);

  float scaleX = length(basisX);
  float scaleY = length(basisY);
  float scaleZ = length(basisZ);

  if ((scaleX <= 0.0F) || (scaleY <= 0.0F) || (scaleZ <= 0.0F)) {
    return false;
  }

  Vec3 normX = div(basisX, scaleX);
  Vec3 normY = div(basisY, scaleY);
  Vec3 normZ = div(basisZ, scaleZ);

  const Vec3 crossXY = cross(normX, normY);
  if (dot(crossXY, normZ) < 0.0F) {
    scaleZ = -scaleZ;
    normZ = mul(normZ, -1.0F);
  }

  Mat4 rotationMat(Vec4(normX.x, normX.y, normX.z, 0.0F),
                   Vec4(normY.x, normY.y, normY.z, 0.0F),
                   Vec4(normZ.x, normZ.y, normZ.z, 0.0F),
                   Vec4(0.0F, 0.0F, 0.0F, 1.0F));

  *outTranslation = translation;
  *outScale = Vec3(scaleX, scaleY, scaleZ);
  *outRotation = from_mat4(rotationMat);
  return true;
}

Mat4 look_at(const Vec3 &eye, const Vec3 &target, const Vec3 &up) noexcept {
  const Vec3 forward = normalize(sub(target, eye));
  const Vec3 right = normalize(cross(forward, up));
  const Vec3 trueUp = cross(right, forward);

  return Mat4(
      Vec4(right.x, trueUp.x, -forward.x, 0.0F),
      Vec4(right.y, trueUp.y, -forward.y, 0.0F),
      Vec4(right.z, trueUp.z, -forward.z, 0.0F),
      Vec4(-dot(right, eye), -dot(trueUp, eye), dot(forward, eye), 1.0F));
}

Mat4 perspective(float verticalFovRadians, float aspect, float nearZ,
                 float farZ) noexcept {
#ifndef NDEBUG
  assert(aspect > 0.0F);
  assert(nearZ > 0.0F);
  assert(farZ > nearZ);
  assert(std::fabs(verticalFovRadians) > 1.0e-6F);
#endif

  const float halfFov = 0.5F * verticalFovRadians;
  const float invTan = 1.0F / std::tan(halfFov);
  const float invDepth = 1.0F / (nearZ - farZ);

  return Mat4(Vec4(invTan / aspect, 0.0F, 0.0F, 0.0F),
              Vec4(0.0F, invTan, 0.0F, 0.0F),
              Vec4(0.0F, 0.0F, (farZ + nearZ) * invDepth, -1.0F),
              Vec4(0.0F, 0.0F, (2.0F * farZ * nearZ) * invDepth, 0.0F));
}

Mat4 ortho(float left, float right, float bottom, float top, float nearZ,
           float farZ) noexcept {
#ifndef NDEBUG
  assert(right > left);
  assert(top > bottom);
  assert(farZ > nearZ);
#endif

  const float invWidth = 1.0F / (right - left);
  const float invHeight = 1.0F / (top - bottom);
  const float invDepth = 1.0F / (farZ - nearZ);

  return Mat4(Vec4(2.0F * invWidth, 0.0F, 0.0F, 0.0F),
              Vec4(0.0F, 2.0F * invHeight, 0.0F, 0.0F),
              Vec4(0.0F, 0.0F, -2.0F * invDepth, 0.0F),
              Vec4(-(right + left) * invWidth, -(top + bottom) * invHeight,
                   -(farZ + nearZ) * invDepth, 1.0F));
}

} // namespace engine::math
