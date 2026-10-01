// TRS composition/decomposition and camera/projection matrix builders,
// defined inline; compose_trs runs per entity per frame.

#pragma once

#include <cassert>
#include <cmath>
#include <cstddef>

#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::math {

/// Builds a column-major model matrix applying scale, then rotation, then
/// translation.
inline Mat4 compose_trs(const Vec3 &translation, const Quat &rotation,
                        const Vec3 &scale) noexcept {
  Mat4 result = to_mat4(rotation);
  result.columns[0] = mul(result.columns[0], scale.x);
  result.columns[1] = mul(result.columns[1], scale.y);
  result.columns[2] = mul(result.columns[2], scale.z);
  result.columns[3] = Vec4(translation.x, translation.y, translation.z, 1.0F);
  return result;
}

/// Largest |cosine| allowed between two basis axes of a matrix taken as
/// TRS: 1e-3 is about 0.06 degrees off square, far above the float
/// round-off of composing, inverting and multiplying TRS matrices, and far
/// below the shear a non-uniformly scaled parent puts on a rotated child.
inline constexpr float kTrsShearTolerance = 1.0e-3F;

/// Smallest basis length taken as a scale; below it the matrix is
/// singular for TRS purposes.
inline constexpr float kTrsMinScale = 1.0e-8F;

/// Splits a matrix into translation, rotation and scale. Returns false,
/// leaving the outputs unset, for a matrix that is not TRS: a non-finite
/// value, a basis axis shorter than kTrsMinScale (singular), or two axes
/// further from perpendicular than kTrsShearTolerance (sheared, as a
/// rotated child of a non-uniformly scaled parent is), since any rotation
/// taken from such a basis is not a rotation. The rotation returned is
/// unit length. A mirrored basis (negative determinant) is accepted and
/// folded into a negative scale.z; a caller that cannot hold a mirror
/// (the skeleton importer) refuses a negative scale.z itself.
inline bool decompose_trs(const Mat4 &value, Vec3 *outTranslation,
                          Quat *outRotation, Vec3 *outScale) noexcept {
  if ((outTranslation == nullptr) || (outRotation == nullptr) ||
      (outScale == nullptr)) {
    return false;
  }

  const Vec3 translation(value.columns[3].x, value.columns[3].y,
                         value.columns[3].z);
  if (!std::isfinite(translation.x) || !std::isfinite(translation.y) ||
      !std::isfinite(translation.z)) {
    return false;
  }
  Vec3 scale(
      length(Vec3(value.columns[0].x, value.columns[0].y, value.columns[0].z)),
      length(Vec3(value.columns[1].x, value.columns[1].y, value.columns[1].z)),
      length(Vec3(value.columns[2].x, value.columns[2].y, value.columns[2].z)));
  // Written so a NaN or infinite length fails too.
  if (!(scale.x > kTrsMinScale) || !(scale.y > kTrsMinScale) ||
      !(scale.z > kTrsMinScale) || !std::isfinite(scale.x) ||
      !std::isfinite(scale.y) || !std::isfinite(scale.z)) {
    return false;
  }

  Mat4 rotationOnly = value;
  rotationOnly.columns[3] = Vec4(0.0F, 0.0F, 0.0F, 1.0F);
  for (std::size_t column = 0U; column < 3U; ++column) {
    const float axisScale = (column == 0U)   ? scale.x
                            : (column == 1U) ? scale.y
                                             : scale.z;
    rotationOnly.columns[column].x /= axisScale;
    rotationOnly.columns[column].y /= axisScale;
    rotationOnly.columns[column].z /= axisScale;
  }

  Vec4 &c0 = rotationOnly.columns[0];
  Vec4 &c1 = rotationOnly.columns[1];
  Vec4 &c2 = rotationOnly.columns[2];
  const float dot01 = (c0.x * c1.x) + (c0.y * c1.y) + (c0.z * c1.z);
  const float dot02 = (c0.x * c2.x) + (c0.y * c2.y) + (c0.z * c2.z);
  const float dot12 = (c1.x * c2.x) + (c1.y * c2.y) + (c1.z * c2.z);
  if ((std::fabs(dot01) > kTrsShearTolerance) ||
      (std::fabs(dot02) > kTrsShearTolerance) ||
      (std::fabs(dot12) > kTrsShearTolerance)) {
    return false;
  }

  const float determinant = c0.x * (c1.y * c2.z - c1.z * c2.y) -
                            c1.x * (c0.y * c2.z - c0.z * c2.y) +
                            c2.x * (c0.y * c1.z - c0.z * c1.y);
  if (determinant < 0.0F) {
    scale.z = -scale.z;
    c2.x = -c2.x;
    c2.y = -c2.y;
    c2.z = -c2.z;
  }

  *outTranslation = translation;
  *outScale = scale;
  *outRotation = normalize(from_mat4(rotationOnly));
  return true;
}

/// The rotation of a matrix's basis with scale and shear removed: the X
/// axis kept, Y made perpendicular to it, Z completing a right-handed
/// frame (Gram-Schmidt). Unlike decompose_trs it accepts a sheared basis,
/// for comparing two poses of one sheared matrix; false for a singular
/// one.
inline bool basis_rotation(const Mat4 &value, Quat *outRotation) noexcept {
  if (outRotation == nullptr) {
    return false;
  }
  const Vec3 x(value.columns[0].x, value.columns[0].y, value.columns[0].z);
  const Vec3 y(value.columns[1].x, value.columns[1].y, value.columns[1].z);
  const float lengthX = length(x);
  if (!(lengthX > kTrsMinScale) || !std::isfinite(lengthX)) {
    return false;
  }
  const Vec3 axisX = div(x, lengthX);
  const Vec3 rejected = sub(y, mul(axisX, dot(y, axisX)));
  const float lengthY = length(rejected);
  if (!(lengthY > kTrsMinScale) || !std::isfinite(lengthY)) {
    return false;
  }
  const Vec3 axisY = div(rejected, lengthY);
  const Vec3 axisZ = cross(axisX, axisY);
  const Mat4 frame(Vec4(axisX.x, axisX.y, axisX.z, 0.0F),
                   Vec4(axisY.x, axisY.y, axisY.z, 0.0F),
                   Vec4(axisZ.x, axisZ.y, axisZ.z, 0.0F),
                   Vec4(0.0F, 0.0F, 0.0F, 1.0F));
  *outRotation = normalize(from_mat4(frame));
  return true;
}

/// Right-handed view matrix looking from eye toward target.
inline Mat4 look_at(const Vec3 &eye, const Vec3 &target,
                    const Vec3 &up) noexcept {
  const Vec3 forward = normalize(sub(target, eye));
  const Vec3 right = normalize(cross(forward, up));
  const Vec3 trueUp = cross(right, forward);

  return Mat4(
      Vec4(right.x, trueUp.x, -forward.x, 0.0F),
      Vec4(right.y, trueUp.y, -forward.y, 0.0F),
      Vec4(right.z, trueUp.z, -forward.z, 0.0F),
      Vec4(-dot(right, eye), -dot(trueUp, eye), dot(forward, eye), 1.0F));
}

/// Right-handed perspective projection with OpenGL clip depth [-1, 1].
inline Mat4 perspective(float verticalFovRadians, float aspect, float nearZ,
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

/// Right-handed perspective projection with clip depth [0, 1]
/// (Vulkan/Metal/D3D convention; the bgfx backend selects it through
/// DeviceCaps::depthZeroToOne when the live API is not GL-family).
inline Mat4 perspective_zero_one(float verticalFovRadians, float aspect,
                                 float nearZ, float farZ) noexcept {
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
              Vec4(0.0F, 0.0F, farZ * invDepth, -1.0F),
              Vec4(0.0F, 0.0F, (farZ * nearZ) * invDepth, 0.0F));
}

/// Right-handed orthographic projection with OpenGL clip depth [-1, 1].
inline Mat4 ortho(float left, float right, float bottom, float top,
                  float nearZ, float farZ) noexcept {
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

/// Right-handed orthographic projection with clip depth [0, 1]
/// (Vulkan/Metal/D3D convention; see perspective_zero_one).
inline Mat4 ortho_zero_one(float left, float right, float bottom, float top,
                           float nearZ, float farZ) noexcept {
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
              Vec4(0.0F, 0.0F, -invDepth, 0.0F),
              Vec4(-(right + left) * invWidth, -(top + bottom) * invHeight,
                   -nearZ * invDepth, 1.0F));
}

} // namespace engine::math
