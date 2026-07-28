// Converts editor gizmo world matrices back into hierarchy-local transforms.

#pragma once

#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// Decomposes a manipulated world matrix into local TRS while retaining
/// non-TRS Transform metadata such as the persistent parent id.
inline bool
world_matrix_to_local_transform(const math::Mat4 &worldMatrix,
                                const math::Mat4 *parentWorldMatrix,
                                const runtime::Transform &currentLocal,
                                runtime::Transform *outLocal) noexcept {
  if (outLocal == nullptr) {
    return false;
  }

  math::Mat4 localMatrix = worldMatrix;
  if (parentWorldMatrix != nullptr) {
    math::Mat4 inverseParent{};
    if (!math::inverse(*parentWorldMatrix, &inverseParent)) {
      return false;
    }
    localMatrix = math::mul(inverseParent, worldMatrix);
  }

  math::Vec3 position{};
  math::Quat rotation{};
  math::Vec3 scale{};
  if (!math::decompose_trs(localMatrix, &position, &rotation, &scale)) {
    return false;
  }

  runtime::Transform result = currentLocal;
  result.position = position;
  result.rotation = rotation;
  result.scale = scale;
  *outLocal = result;
  return true;
}

} // namespace engine::editor
