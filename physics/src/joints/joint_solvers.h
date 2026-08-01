// Declares the joint solve context and the per-type solver entry points
// invoked by the constraint solver's Gauss-Seidel loop. Each solver
// projects its joint's position-level constraints (documented per file
// with the Jacobian and effective-mass derivation) and removes the
// violating relative-velocity components so integration cannot re-violate
// what the projection just corrected.

#pragma once

#include "engine/math/component_types.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics_context.h"

namespace engine::physics {

using engine::math::RigidBody;
using engine::math::Transform;

/// Endpoint state for one joint solve; inverse mass and inertia are the
/// effective values (zero for static or missing bodies).
struct JointSolveContext final {
  Transform *tA = nullptr;
  Transform *tB = nullptr;
  RigidBody *bodyA = nullptr;
  RigidBody *bodyB = nullptr;
  float invMassA = 0.0F;
  float invMassB = 0.0F;
  float invInertiaA = 0.0F;
  float invInertiaB = 0.0F;
};

// Per-type solvers: one Gauss-Seidel iteration of position projection plus
// velocity projection. Each returns the iteration's position-correction
// magnitude and accumulates the slot's impulse diagnostic.
float solve_distance_joint(JointSolveContext &ctx,
                           PhysicsJointSlot &joint) noexcept;

float solve_hinge_joint(JointSolveContext &ctx,
                        PhysicsJointSlot &joint) noexcept;

float solve_ball_socket_joint(JointSolveContext &ctx,
                              PhysicsJointSlot &joint) noexcept;

float solve_slider_joint(JointSolveContext &ctx,
                         PhysicsJointSlot &joint) noexcept;

float solve_spring_joint(JointSolveContext &ctx, PhysicsJointSlot &joint,
                         float deltaSeconds) noexcept;

float solve_fixed_joint(JointSolveContext &ctx,
                        PhysicsJointSlot &joint) noexcept;

} // namespace engine::physics
