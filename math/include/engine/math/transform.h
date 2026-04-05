#pragma once

#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::math {

Mat4 compose_trs(const Vec3 &translation, const Quat &rotation,
                 const Vec3 &scale) noexcept;

bool decompose_trs(const Mat4 &value, Vec3 *outTranslation, Quat *outRotation,
                   Vec3 *outScale) noexcept;

Mat4 look_at(const Vec3 &eye, const Vec3 &target, const Vec3 &up) noexcept;

Mat4 perspective(float verticalFovRadians, float aspect, float nearZ,
                 float farZ) noexcept;

Mat4 ortho(float left, float right, float bottom, float top, float nearZ,
           float farZ) noexcept;

} // namespace engine::math
