#pragma once

#include "engine/math/component_types.h"
#include "engine/math/vec3.h"

namespace engine::physics {

using engine::math::RigidBody;
using engine::math::Transform;

struct JointSolveContext final {
  Transform *tA = nullptr;
  Transform *tB = nullptr;
  RigidBody *bodyA = nullptr;
  RigidBody *bodyB = nullptr;
  float invMassA = 0.0F;
  float invMassB = 0.0F;
};

// Per-type positional correction functions.
// Each applies one iteration of correction and returns the impulse magnitude.
float solve_distance_joint(JointSolveContext &ctx, float targetDistance,
                           float &accumulatedImpulse) noexcept;

float solve_hinge_joint(JointSolveContext &ctx, const math::Vec3 &anchorA,
                        const math::Vec3 &anchorB, const math::Vec3 &axis,
                        bool hasLimits, float minAngle, float maxAngle,
                        float &accumulatedImpulse) noexcept;

float solve_ball_socket_joint(JointSolveContext &ctx, const math::Vec3 &anchorA,
                              const math::Vec3 &anchorB,
                              float &accumulatedImpulse) noexcept;

float solve_slider_joint(JointSolveContext &ctx, const math::Vec3 &axis,
                         bool hasLimits, float minDist, float maxDist,
                         float &accumulatedImpulse) noexcept;

float solve_spring_joint(JointSolveContext &ctx, float restLength,
                         float stiffness, float damping, float deltaSeconds,
                         float &accumulatedImpulse) noexcept;

float solve_fixed_joint(JointSolveContext &ctx, const math::Vec3 &anchorA,
                        const math::Vec3 &anchorB,
                        float &accumulatedImpulse) noexcept;

} // namespace engine::physics
