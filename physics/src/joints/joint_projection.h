// Declares the shared constraint-projection helpers used by every joint
// solver: world-space anchor levers, generalized-inverse-mass point
// projection, anchor relative-velocity removal through the 3x3 anchor mass
// matrix, and inertia-split relative orientation / angular-velocity
// corrections. All helpers treat the engine's scalar inverse inertia as an
// isotropic tensor, matching the contact solver.

#pragma once

#include "engine/math/quat.h"
#include "engine/math/vec3.h"

#include "joint_solvers.h"

namespace engine::physics {

/// World-space lever arm of a body-local anchor under the body's rotation,
/// normalized on use: the public Transform API accepts non-unit
/// quaternions, and rotate_vector requires unit length.
math::Vec3 joint_world_lever(const Transform &transform,
                             const math::Vec3 &localAnchor) noexcept;

/// Rotates a transform by a world-frame rotation vector (axis times angle).
void apply_orientation_delta(Transform &transform,
                             const math::Vec3 &rotVec) noexcept;

/// Projects the anchor-point error pB - pA to zero along its own direction:
/// impulse magnitude |C| / (mA^-1 + mB^-1 + iA|rA x n|^2 + iB|rB x n|^2)
/// applied at the anchors, so offset anchors translate AND rotate their
/// bodies. Returns the applied impulse magnitude.
float project_point_position(JointSolveContext &ctx, const math::Vec3 &leverA,
                             const math::Vec3 &leverB,
                             const math::Vec3 &error) noexcept;

/// Relative velocity of body B's anchor with respect to body A's anchor,
/// v + w x r per side; missing bodies contribute zero.
math::Vec3 relative_anchor_velocity(const JointSolveContext &ctx,
                                    const math::Vec3 &leverA,
                                    const math::Vec3 &leverB) noexcept;

/// Removes the given relative-anchor-velocity component with one impulse
/// solved through the 3x3 anchor mass matrix K = (mA^-1 + mB^-1)I +
/// iA(|rA|^2 I - rA rA^T) + iB(|rB|^2 I - rB rB^T). Returns the impulse
/// magnitude.
float project_point_velocity(JointSolveContext &ctx, const math::Vec3 &leverA,
                             const math::Vec3 &leverB,
                             const math::Vec3 &remove) noexcept;

/// Applies a relative orientation correction: the world rotation vector is
/// added to body B and subtracted from body A, split by inverse inertia
/// (effective inverse mass iA + iB). Returns the applied angle.
float apply_relative_orientation_delta(JointSolveContext &ctx,
                                       const math::Vec3 &rotVec) noexcept;

/// Removes the given component of the relative angular velocity wB - wA
/// with one inertia-split angular impulse. Returns the impulse magnitude.
float project_relative_angular_velocity(JointSolveContext &ctx,
                                        const math::Vec3 &remove) noexcept;

/// Shortest-arc world rotation vector that, added to body B, restores the
/// creation-time relative orientation qB = qA * reference.
math::Vec3 relative_orientation_correction(
    const Transform &transformA, const Transform &transformB,
    const math::Quat &reference) noexcept;

} // namespace engine::physics
