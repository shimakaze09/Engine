// Declares transform types and APIs for the Engine math library.

#pragma once

#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::math {

/// Handles compose trs.
Mat4 compose_trs(const Vec3 &translation, const Quat &rotation,
                 const Vec3 &scale) noexcept;

/// Handles decompose trs.
bool decompose_trs(const Mat4 &value, Vec3 *outTranslation, Quat *outRotation,
                   Vec3 *outScale) noexcept;

/// Handles look at.
Mat4 look_at(const Vec3 &eye, const Vec3 &target, const Vec3 &up) noexcept;

/// Handles perspective.
Mat4 perspective(float verticalFovRadians, float aspect, float nearZ,
                 float farZ) noexcept;

/// Handles ortho.
Mat4 ortho(float left, float right, float bottom, float top, float nearZ,
           float farZ) noexcept;

} // namespace engine::math
