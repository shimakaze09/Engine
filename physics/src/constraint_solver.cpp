#include "engine/physics/constraint_solver.h"

#include "engine/core/cvar.h"
#include "engine/math/vec3.h"
#include "engine/runtime/world.h"
#include "joints/joint_solvers.h"

#include <cstddef>

namespace engine::physics {

namespace {

// CVar: physics.solver_iterations (default 8).
const bool g_solverIterCVar = core::cvar_register_int(
    "physics.solver_iterations", 8, "Number of constraint solver iterations");

} // namespace

// --- Typed joint creation ---------------------------------------------------

static JointId allocate_joint(runtime::World &world) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  for (std::size_t i = 0U; i < runtime::World::kMaxPhysicsJoints; ++i) {
    if (!ctx.joints[i].active) {
      if (i >= ctx.jointCount) {
        ctx.jointCount = i + 1U;
      }
      return static_cast<JointId>(i);
    }
  }
  return kInvalidJointId;
}

JointId add_hinge_joint(runtime::World &world, runtime::Entity entityA,
                        runtime::Entity entityB, const math::Vec3 &pivot,
                        const math::Vec3 &axis) noexcept {
  const JointId id = allocate_joint(world);
  if (id == kInvalidJointId) {
    return id;
  }

  runtime::World::PhysicsContext &ctx = world.physics_context();
  auto &j = ctx.joints[id];
  j.entityA = entityA;
  j.entityB = entityB;
  j.type = runtime::World::JointType::Hinge;
  j.active = true;

  // Compute local anchors from pivot (world-space).
  runtime::Transform tA{};
  runtime::Transform tB{};
  world.get_transform(entityA, &tA);
  world.get_transform(entityB, &tB);
  j.anchorA = math::sub(pivot, tA.position);
  j.anchorB = math::sub(pivot, tB.position);
  j.axis = axis;
  j.accumulatedImpulse = 0.0F;

  return id;
}

JointId add_ball_socket_joint(runtime::World &world, runtime::Entity entityA,
                              runtime::Entity entityB,
                              const math::Vec3 &pivot) noexcept {
  const JointId id = allocate_joint(world);
  if (id == kInvalidJointId) {
    return id;
  }

  runtime::World::PhysicsContext &ctx = world.physics_context();
  auto &j = ctx.joints[id];
  j.entityA = entityA;
  j.entityB = entityB;
  j.type = runtime::World::JointType::BallSocket;
  j.active = true;

  runtime::Transform tA{};
  runtime::Transform tB{};
  world.get_transform(entityA, &tA);
  world.get_transform(entityB, &tB);
  j.anchorA = math::sub(pivot, tA.position);
  j.anchorB = math::sub(pivot, tB.position);
  j.accumulatedImpulse = 0.0F;

  return id;
}

JointId add_slider_joint(runtime::World &world, runtime::Entity entityA,
                         runtime::Entity entityB,
                         const math::Vec3 &axis) noexcept {
  const JointId id = allocate_joint(world);
  if (id == kInvalidJointId) {
    return id;
  }

  runtime::World::PhysicsContext &ctx = world.physics_context();
  auto &j = ctx.joints[id];
  j.entityA = entityA;
  j.entityB = entityB;
  j.type = runtime::World::JointType::Slider;
  j.active = true;
  j.axis = axis;
  j.accumulatedImpulse = 0.0F;

  return id;
}

JointId add_spring_joint(runtime::World &world, runtime::Entity entityA,
                         runtime::Entity entityB, float restLength,
                         float stiffness, float damping) noexcept {
  const JointId id = allocate_joint(world);
  if (id == kInvalidJointId) {
    return id;
  }

  runtime::World::PhysicsContext &ctx = world.physics_context();
  auto &j = ctx.joints[id];
  j.entityA = entityA;
  j.entityB = entityB;
  j.type = runtime::World::JointType::Spring;
  j.active = true;
  j.distance = restLength;
  j.stiffness = stiffness;
  j.damping = damping;
  j.accumulatedImpulse = 0.0F;

  return id;
}

JointId add_fixed_joint(runtime::World &world, runtime::Entity entityA,
                        runtime::Entity entityB) noexcept {
  const JointId id = allocate_joint(world);
  if (id == kInvalidJointId) {
    return id;
  }

  runtime::World::PhysicsContext &ctx = world.physics_context();
  auto &j = ctx.joints[id];
  j.entityA = entityA;
  j.entityB = entityB;
  j.type = runtime::World::JointType::Fixed;
  j.active = true;

  runtime::Transform tA{};
  runtime::Transform tB{};
  world.get_transform(entityA, &tA);
  world.get_transform(entityB, &tB);
  j.anchorA = math::sub(tB.position, tA.position);
  j.anchorB = math::Vec3(0.0F, 0.0F, 0.0F);
  j.accumulatedImpulse = 0.0F;

  return id;
}

void set_joint_limits(runtime::World &world, JointId id, float minLimit,
                      float maxLimit) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if (id >= runtime::World::kMaxPhysicsJoints) {
    return;
  }
  auto &j = ctx.joints[id];
  if (!j.active) {
    return;
  }
  j.hasLimits = true;
  j.minLimit = minLimit;
  j.maxLimit = maxLimit;
}

// --- Main constraint solver -------------------------------------------------

void solve_constraints(runtime::World &world, float deltaSeconds) noexcept {
  const auto simToken = world.simulation_access_token();
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if (ctx.jointCount == 0U) {
    return;
  }

  const int iterations = core::cvar_get_int("physics.solver_iterations");
  const std::size_t iterCount =
      (iterations > 0) ? static_cast<std::size_t>(iterations) : 8U;

  // Warm starting: apply accumulated impulses from previous frame.
  for (std::size_t i = 0U; i < ctx.jointCount; ++i) {
    if (!ctx.joints[i].active) {
      continue;
    }
    auto &j = ctx.joints[i];
    if (j.accumulatedImpulse <= 0.0F) {
      continue;
    }

    runtime::Transform *tA =
        world.get_transform_write_ptr(j.entityA, simToken);
    runtime::Transform *tB =
        world.get_transform_write_ptr(j.entityB, simToken);
    if ((tA == nullptr) || (tB == nullptr)) {
      continue;
    }

    runtime::RigidBody *bodyA = world.get_rigid_body_ptr(j.entityA);
    runtime::RigidBody *bodyB = world.get_rigid_body_ptr(j.entityB);
    const float invMassA = (bodyA != nullptr) ? bodyA->inverseMass : 0.0F;
    const float invMassB = (bodyB != nullptr) ? bodyB->inverseMass : 0.0F;
    const float invMassSum = invMassA + invMassB;
    if (invMassSum <= 0.0F) {
      continue;
    }

    // Apply fraction of previous accumulated impulse as warm start.
    const math::Vec3 delta = math::sub(tB->position, tA->position);
    const float dist = math::length(delta);
    if (dist < 1e-8F) {
      continue;
    }
    const math::Vec3 dir = math::div(delta, dist);
    const float warmImpulse = j.accumulatedImpulse * 0.8F; // 80% warm start
    tA->position = math::add(tA->position,
                              math::mul(dir, warmImpulse * invMassA / invMassSum));
    tB->position = math::sub(tB->position,
                              math::mul(dir, warmImpulse * invMassB / invMassSum));
  }

  // Reset accumulated impulses for this frame.
  for (std::size_t i = 0U; i < ctx.jointCount; ++i) {
    ctx.joints[i].accumulatedImpulse = 0.0F;
  }

  // Iterative solving.
  for (std::size_t iter = 0U; iter < iterCount; ++iter) {
    for (std::size_t i = 0U; i < ctx.jointCount; ++i) {
      if (!ctx.joints[i].active) {
        continue;
      }

      auto &j = ctx.joints[i];

      runtime::Transform *tA =
          world.get_transform_write_ptr(j.entityA, simToken);
      runtime::Transform *tB =
          world.get_transform_write_ptr(j.entityB, simToken);
      if ((tA == nullptr) || (tB == nullptr)) {
        continue;
      }

      runtime::RigidBody *bodyA = world.get_rigid_body_ptr(j.entityA);
      runtime::RigidBody *bodyB = world.get_rigid_body_ptr(j.entityB);

      JointSolveContext solveCtx{};
      solveCtx.tA = tA;
      solveCtx.tB = tB;
      solveCtx.bodyA = bodyA;
      solveCtx.bodyB = bodyB;
      solveCtx.invMassA = (bodyA != nullptr) ? bodyA->inverseMass : 0.0F;
      solveCtx.invMassB = (bodyB != nullptr) ? bodyB->inverseMass : 0.0F;

      const auto jointType =
          static_cast<runtime::World::JointType>(j.type);

      switch (jointType) {
      case runtime::World::JointType::Distance:
        solve_distance_joint(solveCtx, j.distance, j.accumulatedImpulse);
        break;
      case runtime::World::JointType::Hinge:
        solve_hinge_joint(solveCtx, j.anchorA, j.anchorB, j.axis,
                          j.hasLimits, j.minLimit, j.maxLimit,
                          j.accumulatedImpulse);
        break;
      case runtime::World::JointType::BallSocket:
        solve_ball_socket_joint(solveCtx, j.anchorA, j.anchorB,
                                j.accumulatedImpulse);
        break;
      case runtime::World::JointType::Slider:
        solve_slider_joint(solveCtx, j.axis, j.hasLimits, j.minLimit,
                           j.maxLimit, j.accumulatedImpulse);
        break;
      case runtime::World::JointType::Spring:
        solve_spring_joint(solveCtx, j.distance, j.stiffness, j.damping,
                           deltaSeconds, j.accumulatedImpulse);
        break;
      case runtime::World::JointType::Fixed:
        solve_fixed_joint(solveCtx, j.anchorA, j.anchorB,
                          j.accumulatedImpulse);
        break;
      }
    }
  }
}

} // namespace engine::physics
