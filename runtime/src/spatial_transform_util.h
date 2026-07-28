// Provides shared world-space conversion helpers for runtime spatial consumers.

#pragma once

#include <cmath>

#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::runtime::detail {

/// Rotates a component-local direction into world space without changing its
/// authored magnitude.
inline math::Vec3
rotate_local_direction(const math::Quat &worldRotation,
                       const math::Vec3 &localDirection) noexcept {
  return math::rotate_vector(localDirection, worldRotation);
}

/// Returns the conservative world-axis half extents of a local AABB after the
/// complete affine world matrix is applied.
inline math::Vec3
transformed_aabb_half_extents(const math::Mat4 &worldMatrix,
                              const math::Vec3 &localHalfExtents) noexcept {
  const float halfX = std::fabs(localHalfExtents.x);
  const float halfY = std::fabs(localHalfExtents.y);
  const float halfZ = std::fabs(localHalfExtents.z);

  return math::Vec3((std::fabs(worldMatrix.columns[0].x) * halfX) +
                        (std::fabs(worldMatrix.columns[1].x) * halfY) +
                        (std::fabs(worldMatrix.columns[2].x) * halfZ),
                    (std::fabs(worldMatrix.columns[0].y) * halfX) +
                        (std::fabs(worldMatrix.columns[1].y) * halfY) +
                        (std::fabs(worldMatrix.columns[2].y) * halfZ),
                    (std::fabs(worldMatrix.columns[0].z) * halfX) +
                        (std::fabs(worldMatrix.columns[1].z) * halfY) +
                        (std::fabs(worldMatrix.columns[2].z) * halfZ));
}

} // namespace engine::runtime::detail
