// Declares physics types types and APIs for the Engine physics system.

#pragma once

// Convenience header for the physics module.
// Re-exports shared types from core/math into engine::physics namespace
// and adds physics-specific result types (PhysicsRaycastHit).

#include "engine/core/entity.h"
#include "engine/math/component_types.h"
#include "engine/math/mat4.h"

namespace engine::physics {

// Re-export shared types into engine::physics for use in physics
// headers/sources.
using engine::core::Entity;
using engine::core::kInvalidEntity;
using engine::core::kInvalidPersistentId;
using engine::core::PersistentId;
using engine::math::Collider;
using engine::math::ColliderShape;
using engine::math::MovementAuthority;
using engine::math::RigidBody;
using engine::math::Transform;

/// Full world-space transform consumed by physics spatial queries.
///
/// The matrix is authoritative because a rotated child below a non-uniformly
/// scaled parent can contain shear that separate rotation/scale values cannot
/// represent exactly. The decomposed values are retained for body integration
/// and diagnostics.
struct PhysicsTransform final {
  math::Vec3 position = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat rotation = math::Quat();
  math::Vec3 scale = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Mat4 matrix = math::Mat4();
};

// Physics-specific result type.
struct PhysicsRaycastHit final {
  Entity entity = kInvalidEntity;
  float distance = 0.0F;
  math::Vec3 point = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 normal = math::Vec3(0.0F, 0.0F, 0.0F);
};

} // namespace engine::physics
