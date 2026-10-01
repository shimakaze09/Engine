// Converts editor world-space edits back into hierarchy-local transforms:
// a whole world matrix (paste), a world pose kept under a new parent
// (reparent), and one gizmo channel (move, rotate, scale) at a time.

#pragma once

#include <cmath>

#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/physics/physics_types.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// Decomposes a world matrix into local TRS under `parentWorldMatrix`
/// (none for a root) while retaining non-TRS Transform metadata such as
/// the persistent parent id. False when the local matrix is not TRS
/// (math::decompose_trs: sheared, singular or non-finite), leaving
/// `outLocal` unset.
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

/// A world point in the space of `parent` (none: the world itself).
/// False when the parent's matrix cannot be inverted (a zero scale).
inline bool local_point_under(const physics::PhysicsTransform *parent,
                              const math::Vec3 &worldPoint,
                              math::Vec3 *outLocal) noexcept {
  if (parent == nullptr) {
    *outLocal = worldPoint;
    return true;
  }
  math::Mat4 inverseParent{};
  if (!math::inverse(parent->matrix, &inverseParent)) {
    return false;
  }
  *outLocal = math::transform_point(inverseParent, worldPoint);
  return true;
}

/// A world rotation relative to `parent`'s world rotation, the product of
/// the local rotations above it, as Unity's localRotation is: always unit
/// length, whatever the parent's scale.
inline math::Quat
local_rotation_under(const physics::PhysicsTransform *parent,
                     const math::Quat &worldRotation) noexcept {
  return (parent == nullptr)
             ? math::normalize(worldRotation)
             : math::normalize(
                   math::mul(math::conjugate(parent->rotation), worldRotation));
}

/// The local transform that keeps `child`'s world pose under `parent`
/// (none: a root), channel by channel as Unity's SetParent does: the
/// position through the parent's inverse matrix, the rotation relative to
/// the parent's world rotation, the scale divided by the parent's. Under a
/// rotated, non-uniformly scaled parent the child is sheared, which no
/// local TRS can hold; position and rotation are still exact and the
/// scale approximates it. False for a parent with a zero scale axis.
inline bool
local_transform_keeping_world_pose(const physics::PhysicsTransform &child,
                                   const physics::PhysicsTransform *parent,
                                   const runtime::Transform &current,
                                   runtime::Transform *outLocal) noexcept {
  runtime::Transform result = current;
  if (!local_point_under(parent, child.position, &result.position)) {
    return false;
  }
  result.rotation = local_rotation_under(parent, child.rotation);
  result.scale = child.scale;
  if (parent != nullptr) {
    const math::Vec3 &s = parent->scale;
    if ((s.x == 0.0F) || (s.y == 0.0F) || (s.z == 0.0F)) {
      return false;
    }
    result.scale = math::Vec3(child.scale.x / s.x, child.scale.y / s.y,
                              child.scale.z / s.z);
  }
  *outLocal = result;
  return true;
}

/// The one channel a gizmo drag changes.
enum class GizmoChannel { Translate, Rotate, Scale };

/// Applies one gizmo step to an entity's local transform, writing only
/// the channel the gizmo changed, as Unity's tools do: every other channel
/// keeps its value, so rotating a child of a non-uniformly scaled parent
/// never rewrites its scale. `before` is the world matrix handed to the
/// gizmo and `after` the one it returned; `entityWorld` is the entity's
/// world pose before the step and `parent` its parent's (none: a root).
/// - Translate: the new world position, through the parent's inverse.
/// - Rotate: the step's world-space rotation (between the two matrices'
///   bases) applied to the entity's world rotation, made local; unit
///   length always.
/// - Scale: each local axis scaled by how much the gizmo stretched it.
/// False for a degenerate input (a singular basis, a zero-scale parent),
/// leaving `outLocal` unset.
inline bool gizmo_step_to_local(GizmoChannel channel, const math::Mat4 &before,
                                const math::Mat4 &after,
                                const physics::PhysicsTransform &entityWorld,
                                const physics::PhysicsTransform *parent,
                                const runtime::Transform &current,
                                runtime::Transform *outLocal) noexcept {
  runtime::Transform result = current;
  switch (channel) {
  case GizmoChannel::Translate: {
    const math::Vec3 worldPosition(after.columns[3].x, after.columns[3].y,
                                   after.columns[3].z);
    if (!local_point_under(parent, worldPosition, &result.position)) {
      return false;
    }
    break;
  }
  case GizmoChannel::Rotate: {
    math::Quat from{};
    math::Quat to{};
    if (!math::basis_rotation(before, &from) ||
        !math::basis_rotation(after, &to)) {
      return false;
    }
    const math::Quat step = math::mul(to, math::conjugate(from));
    result.rotation = local_rotation_under(
        parent, math::normalize(math::mul(step, entityWorld.rotation)));
    break;
  }
  case GizmoChannel::Scale: {
    float ratio[3] = {};
    for (int axis = 0; axis < 3; ++axis) {
      const math::Vec4 &a = before.columns[axis];
      const math::Vec4 &b = after.columns[axis];
      const float from = math::length(math::Vec3(a.x, a.y, a.z));
      const float to = math::length(math::Vec3(b.x, b.y, b.z));
      if (!(from > math::kTrsMinScale) || !std::isfinite(to)) {
        return false;
      }
      ratio[axis] = to / from;
    }
    result.scale =
        math::Vec3(current.scale.x * ratio[0], current.scale.y * ratio[1],
                   current.scale.z * ratio[2]);
    break;
  }
  }
  *outLocal = result;
  return true;
}

} // namespace engine::editor
