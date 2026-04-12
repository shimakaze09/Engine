#pragma once

#include "engine/math/vec3.h"

namespace engine::runtime {
class World;
struct Transform;
struct RigidBody;
struct Entity;
} // namespace engine::runtime

namespace engine::physics {

struct JointSolveContext final {
  runtime::Transform *tA = nullptr;
  runtime::Transform *tB = nullptr;
  runtime::RigidBody *bodyA = nullptr;
  runtime::RigidBody *bodyB = nullptr;
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
