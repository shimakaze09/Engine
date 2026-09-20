// Rigid-body mass properties: derives a body-space inverse inertia tensor
// from collider geometry and applies it in world space, so contacts,
// joints and integration share one rotational-mass model.

#pragma once

#include "engine/math/component_types.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::physics {

/// Sums the body-space inertia tensor of several colliders sharing one
/// body. Each collider is added at its offset and rotation relative to the
/// body origin; the mass is split between colliders by volume times
/// density. Only the diagonal of the summed tensor is kept: a collider
/// rotated off the body axes contributes its rotated diagonal.
struct InertiaAccumulator final {
  math::Vec3 inertia = math::Vec3(0.0F, 0.0F, 0.0F);
  float weightSum = 0.0F;
};

/// Adds one collider's unit-mass-weight contribution to the accumulator.
/// `offset` and `rotation` place the collider's own frame in the body's
/// frame (the collider's local placement composed with any child transform).
void accumulate_collider_inertia(InertiaAccumulator *accumulator,
                                 const math::Collider &collider,
                                 const math::Vec3 &offset,
                                 const math::Quat &rotation) noexcept;

/// Inverse inertia of the accumulated colliders for a body of the given
/// inverse mass: each axis is 1 / I about that axis, clamped to
/// kMaxInverseInertia; a zero-inertia axis (degenerate geometry) or a
/// static body (inverse mass zero) answers zero on every axis.
[[nodiscard]] math::Vec3
finish_inverse_inertia(const InertiaAccumulator &accumulator,
                       float inverseMass) noexcept;

/// Inverse inertia of a body whose only collider is `collider`, placed at
/// the body origin with the collider's own local placement.
[[nodiscard]] math::Vec3
inverse_inertia_for_collider(const math::Collider &collider,
                             float inverseMass) noexcept;

/// Applies the world-space inverse inertia tensor to `v`:
/// R * diag(inverseInertia) * R^T * v for a unit `rotation`.
[[nodiscard]] math::Vec3 apply_inverse_inertia(const math::Vec3 &inverseInertia,
                                               const math::Quat &rotation,
                                               const math::Vec3 &v) noexcept;

/// Effective inverse mass of an impulse along `direction` applied at lever
/// `r`: (r x d) . I_world^-1 (r x d). Zero when the body cannot rotate.
[[nodiscard]] float angular_effective_inverse_mass(
    const math::Vec3 &inverseInertia, const math::Quat &rotation,
    const math::Vec3 &lever, const math::Vec3 &direction) noexcept;

/// Writes the world-space inverse inertia tensor R * diag * R^T for a unit
/// `rotation` as a row-major 3x3 matrix.
void inverse_inertia_world(const math::Vec3 &inverseInertia,
                           const math::Quat &rotation,
                           float out[3][3]) noexcept;

} // namespace engine::physics
