// Implements constraint solver behavior for the Engine physics system.

#include "engine/physics/constraint_solver.h"

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"
#include "joint_handle.h"
#include "joints/joint_solvers.h"

#include <cmath>
#include <cstddef>

namespace engine::physics {

// --- Typed joint creation ---------------------------------------------------

/// True when every component is finite.
static bool is_finite_joint_vector(const math::Vec3 &value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

/// Reads exact generation-bearing endpoint transforms and rejects self-joints.
static bool read_joint_endpoints(PhysicsWorldView &world, Entity entityA,
                                 Entity entityB, Transform *outTransformA,
                                 Transform *outTransformB) noexcept {
  return (outTransformA != nullptr) && (outTransformB != nullptr) &&
         (entityA != kInvalidEntity) && (entityB != kInvalidEntity) &&
         (entityA != entityB) && world.get_transform(entityA, outTransformA) &&
         world.get_transform(entityB, outTransformB);
}

/// Logs an invalid joint request and returns the sentinel ID.
static JointId reject_joint(const char *message) noexcept {
  core::log_message(core::LogLevel::Error, "physics", message);
  return kInvalidJointId;
}

/// Claims one inactive slot and returns its generation-bearing ID.
static JointId allocate_joint(PhysicsWorldView &world,
                              PhysicsJointSlot **outSlot) noexcept {
  return claim_joint_slot(world.physics_context(), outSlot);
}

JointId add_hinge_joint(PhysicsWorldView &world, Entity entityA, Entity entityB,
                        const math::Vec3 &pivot,
                        const math::Vec3 &axis) noexcept {
  Transform transformA{};
  Transform transformB{};
  const float axisLengthSquared = math::dot(axis, axis);
  if (!is_finite_joint_vector(pivot) || !is_finite_joint_vector(axis) ||
      !std::isfinite(axisLengthSquared) || (axisLengthSquared <= 1.0e-12F) ||
      !read_joint_endpoints(world, entityA, entityB, &transformA,
                            &transformB)) {
    return reject_joint("invalid hinge joint endpoints, pivot, or axis");
  }

  PhysicsJointSlot *joint = nullptr;
  const JointId id = allocate_joint(world, &joint);
  if ((id == kInvalidJointId) || (joint == nullptr)) {
    return reject_joint("joint table full");
  }

  joint->entityA = entityA;
  joint->entityB = entityB;
  joint->type = JointType::Hinge;
  joint->active = true;
  joint->anchorA = math::sub(pivot, transformA.position);
  joint->anchorB = math::sub(pivot, transformB.position);
  joint->axis = math::normalize(axis);
  joint->accumulatedImpulse = 0.0F;
  return id;
}

JointId add_ball_socket_joint(PhysicsWorldView &world, Entity entityA,
                              Entity entityB,
                              const math::Vec3 &pivot) noexcept {
  Transform transformA{};
  Transform transformB{};
  if (!is_finite_joint_vector(pivot) ||
      !read_joint_endpoints(world, entityA, entityB, &transformA,
                            &transformB)) {
    return reject_joint("invalid ball-socket joint endpoints or pivot");
  }

  PhysicsJointSlot *joint = nullptr;
  const JointId id = allocate_joint(world, &joint);
  if ((id == kInvalidJointId) || (joint == nullptr)) {
    return reject_joint("joint table full");
  }

  joint->entityA = entityA;
  joint->entityB = entityB;
  joint->type = JointType::BallSocket;
  joint->active = true;
  joint->anchorA = math::sub(pivot, transformA.position);
  joint->anchorB = math::sub(pivot, transformB.position);
  joint->accumulatedImpulse = 0.0F;
  return id;
}

JointId add_slider_joint(PhysicsWorldView &world, Entity entityA,
                         Entity entityB, const math::Vec3 &axis) noexcept {
  Transform transformA{};
  Transform transformB{};
  const float axisLengthSquared = math::dot(axis, axis);
  if (!is_finite_joint_vector(axis) || !std::isfinite(axisLengthSquared) ||
      (axisLengthSquared <= 1.0e-12F) ||
      !read_joint_endpoints(world, entityA, entityB, &transformA,
                            &transformB)) {
    return reject_joint("invalid slider joint endpoints or axis");
  }

  PhysicsJointSlot *joint = nullptr;
  const JointId id = allocate_joint(world, &joint);
  if ((id == kInvalidJointId) || (joint == nullptr)) {
    return reject_joint("joint table full");
  }

  joint->entityA = entityA;
  joint->entityB = entityB;
  joint->type = JointType::Slider;
  joint->active = true;
  joint->axis = math::normalize(axis);
  joint->accumulatedImpulse = 0.0F;
  return id;
}

JointId add_spring_joint(PhysicsWorldView &world, Entity entityA,
                         Entity entityB, float restLength, float stiffness,
                         float damping) noexcept {
  Transform transformA{};
  Transform transformB{};
  if (!std::isfinite(restLength) || !std::isfinite(stiffness) ||
      !std::isfinite(damping) || (restLength < 0.0F) ||
      (stiffness < 0.0F) || (damping < 0.0F) ||
      !read_joint_endpoints(world, entityA, entityB, &transformA,
                            &transformB)) {
    return reject_joint("invalid spring joint endpoints or parameters");
  }

  PhysicsJointSlot *joint = nullptr;
  const JointId id = allocate_joint(world, &joint);
  if ((id == kInvalidJointId) || (joint == nullptr)) {
    return reject_joint("joint table full");
  }

  joint->entityA = entityA;
  joint->entityB = entityB;
  joint->type = JointType::Spring;
  joint->active = true;
  joint->distance = restLength;
  joint->stiffness = stiffness;
  joint->damping = damping;
  joint->accumulatedImpulse = 0.0F;
  return id;
}

JointId add_fixed_joint(PhysicsWorldView &world, Entity entityA,
                        Entity entityB) noexcept {
  Transform transformA{};
  Transform transformB{};
  if (!read_joint_endpoints(world, entityA, entityB, &transformA,
                            &transformB)) {
    return reject_joint("invalid fixed joint endpoints");
  }

  PhysicsJointSlot *joint = nullptr;
  const JointId id = allocate_joint(world, &joint);
  if ((id == kInvalidJointId) || (joint == nullptr)) {
    return reject_joint("joint table full");
  }

  joint->entityA = entityA;
  joint->entityB = entityB;
  joint->type = JointType::Fixed;
  joint->active = true;
  joint->anchorA = math::sub(transformB.position, transformA.position);
  joint->anchorB = math::Vec3(0.0F, 0.0F, 0.0F);
  joint->accumulatedImpulse = 0.0F;
  return id;
}

/// Sets finite ordered limits on hinge and slider joints.
void set_joint_limits(PhysicsWorldView &world, JointId id, float minLimit,
                      float maxLimit) noexcept {
  PhysicsJointSlot *joint = find_joint_slot(world.physics_context(), id);
  if ((joint == nullptr) ||
      ((joint->type != JointType::Hinge) &&
       (joint->type != JointType::Slider)) ||
      !std::isfinite(minLimit) || !std::isfinite(maxLimit) ||
      (minLimit > maxLimit)) {
    core::log_message(core::LogLevel::Error, "physics",
                      "invalid joint ID, type, or limits");
    return;
  }

  joint->hasLimits = true;
  joint->minLimit = minLimit;
  joint->maxLimit = maxLimit;
}

// --- Main constraint solver -------------------------------------------------

/// Retires constraints whose exact generation-bearing endpoint disappeared.
static void retire_missing_joint_endpoints(PhysicsWorldView &world,
                                           PhysicsContext &context) noexcept {
  Transform endpoint{};
  for (std::size_t index = 0U; index < context.jointCount; ++index) {
    PhysicsJointSlot &joint = context.joints[index];
    if (!joint.active) {
      continue;
    }
    if (!world.get_transform(joint.entityA, &endpoint) ||
        !world.get_transform(joint.entityB, &endpoint)) {
      retire_joint_slot(joint);
    }
  }

  while ((context.jointCount > 0U) &&
         !context.joints[context.jointCount - 1U].active) {
    --context.jointCount;
  }
}

/// Iteratively solves all active joints for the step. Warm starting replays
/// 80% of the previous frame's accumulated impulse along the body center
/// line, and only distance and spring joints participate — the other joint
/// types store anchor/axis-relative magnitudes, so a center-line replay
/// would displace them in directions their solvers never correct.
void solve_constraints(PhysicsWorldView &world, float deltaSeconds) noexcept {
  const auto simToken = world.simulation_access_token();
  PhysicsContext &ctx = world.physics_context();
  retire_missing_joint_endpoints(world, ctx);
  if (ctx.jointCount == 0U) {
    return;
  }

  const int iterations = core::cvar_get_int("physics.solver_iterations");
  const std::size_t iterCount =
      (iterations > 0) ? static_cast<std::size_t>(iterations) : 8U;

  for (std::size_t i = 0U; i < ctx.jointCount; ++i) {
    if (!ctx.joints[i].active) {
      continue;
    }
    auto &j = ctx.joints[i];
    const auto warmType = static_cast<JointType>(j.type);
    if ((warmType != JointType::Distance) && (warmType != JointType::Spring)) {
      continue;
    }
    if (j.accumulatedImpulse == 0.0F) {
      continue;
    }

    Transform *tA = world.get_transform_write_ptr(j.entityA, simToken);
    Transform *tB = world.get_transform_write_ptr(j.entityB, simToken);
    if ((tA == nullptr) || (tB == nullptr)) {
      continue;
    }

    RigidBody *bodyA = world.get_rigid_body_ptr(j.entityA);
    RigidBody *bodyB = world.get_rigid_body_ptr(j.entityB);
    const float invMassA = (bodyA != nullptr) ? bodyA->inverseMass : 0.0F;
    const float invMassB = (bodyB != nullptr) ? bodyB->inverseMass : 0.0F;
    const float invMassSum = invMassA + invMassB;
    if (invMassSum <= 0.0F) {
      continue;
    }

    const math::Vec3 delta = math::sub(tB->position, tA->position);
    const float dist = math::length(delta);
    if (dist < 1e-8F) {
      continue;
    }
    const math::Vec3 dir = math::div(delta, dist);
    const float warmImpulse = j.accumulatedImpulse * 0.8F;
    tA->position = math::add(
        tA->position, math::mul(dir, warmImpulse * invMassA / invMassSum));
    tB->position = math::sub(
        tB->position, math::mul(dir, warmImpulse * invMassB / invMassSum));
  }

  for (std::size_t i = 0U; i < ctx.jointCount; ++i) {
    ctx.joints[i].accumulatedImpulse = 0.0F;
  }

  for (std::size_t iter = 0U; iter < iterCount; ++iter) {
    for (std::size_t i = 0U; i < ctx.jointCount; ++i) {
      if (!ctx.joints[i].active) {
        continue;
      }

      auto &j = ctx.joints[i];

      Transform *tA = world.get_transform_write_ptr(j.entityA, simToken);
      Transform *tB = world.get_transform_write_ptr(j.entityB, simToken);
      if ((tA == nullptr) || (tB == nullptr)) {
        continue;
      }

      RigidBody *bodyA = world.get_rigid_body_ptr(j.entityA);
      RigidBody *bodyB = world.get_rigid_body_ptr(j.entityB);

      JointSolveContext solveCtx{};
      solveCtx.tA = tA;
      solveCtx.tB = tB;
      solveCtx.bodyA = bodyA;
      solveCtx.bodyB = bodyB;
      solveCtx.invMassA = (bodyA != nullptr) ? bodyA->inverseMass : 0.0F;
      solveCtx.invMassB = (bodyB != nullptr) ? bodyB->inverseMass : 0.0F;

      const auto jointType = static_cast<JointType>(j.type);

      switch (jointType) {
      case JointType::Distance:
        solve_distance_joint(solveCtx, j.distance, j.accumulatedImpulse);
        break;
      case JointType::Hinge:
        solve_hinge_joint(solveCtx, j.anchorA, j.anchorB, j.axis, j.hasLimits,
                          j.minLimit, j.maxLimit, j.accumulatedImpulse);
        break;
      case JointType::BallSocket:
        solve_ball_socket_joint(solveCtx, j.anchorA, j.anchorB,
                                j.accumulatedImpulse);
        break;
      case JointType::Slider:
        solve_slider_joint(solveCtx, j.axis, j.hasLimits, j.minLimit,
                           j.maxLimit, j.accumulatedImpulse);
        break;
      case JointType::Spring:
        solve_spring_joint(solveCtx, j.distance, j.stiffness, j.damping,
                           deltaSeconds, j.accumulatedImpulse);
        break;
      case JointType::Fixed:
        solve_fixed_joint(solveCtx, j.anchorA, j.anchorB, j.accumulatedImpulse);
        break;
      }
    }
  }
}

} // namespace engine::physics
