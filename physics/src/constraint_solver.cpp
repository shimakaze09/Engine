// Implements constraint solver behavior for the Engine physics system.

#include "engine/physics/constraint_solver.h"

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/math/quat.h"
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

/// Reads exact generation-bearing endpoint poses and rejects self-joints.
/// Endpoint frames are the hierarchy-COMPOSED world poses, matching what
/// colliders and queries consume, so a parented endpoint's joint frame is
/// captured where the entity actually sits (scale is not consumed: anchors
/// are stored in world metres derived from the world-space pivot).
static bool read_joint_endpoints(PhysicsWorldView &world, Entity entityA,
                                 Entity entityB, Transform *outTransformA,
                                 Transform *outTransformB) noexcept {
  if ((outTransformA == nullptr) || (outTransformB == nullptr) ||
      (entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      (entityA == entityB)) {
    return false;
  }
  PhysicsTransform composedA{};
  PhysicsTransform composedB{};
  if (!world.get_physics_transform(entityA, &composedA) ||
      !world.get_physics_transform(entityB, &composedB)) {
    return false;
  }
  *outTransformA = Transform{};
  outTransformA->position = composedA.position;
  outTransformA->rotation = composedA.rotation;
  *outTransformB = Transform{};
  outTransformB->position = composedB.position;
  outTransformB->rotation = composedB.rotation;
  return true;
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

/// Maps a world offset from a body's origin into that body's local frame.
/// The rotation is normalized on use: the public Transform API accepts
/// non-unit quaternions and rotate_vector requires unit length.
static math::Vec3 to_body_local(const Transform &transform,
                                const math::Vec3 &worldOffset) noexcept {
  return math::rotate_vector(
      worldOffset, math::conjugate(math::normalize(transform.rotation)));
}

/// Creation-time relative orientation of B in A's frame (qA^-1 qB), unit
/// length regardless of the input quaternions' scale.
static math::Quat relative_rotation(const Transform &transformA,
                                    const Transform &transformB) noexcept {
  return math::normalize(
      math::mul(math::conjugate(math::normalize(transformA.rotation)),
                math::normalize(transformB.rotation)));
}

/// Any unit vector perpendicular to a unit axis, from its least-aligned
/// basis vector; seeds the hinge twist references.
static math::Vec3 perpendicular_reference(const math::Vec3 &axis) noexcept {
  const float absX = (axis.x < 0.0F) ? -axis.x : axis.x;
  const float absY = (axis.y < 0.0F) ? -axis.y : axis.y;
  const float absZ = (axis.z < 0.0F) ? -axis.z : axis.z;
  math::Vec3 basis(1.0F, 0.0F, 0.0F);
  if ((absY <= absX) && (absY <= absZ)) {
    basis = math::Vec3(0.0F, 1.0F, 0.0F);
  } else if (absZ <= absX) {
    basis = math::Vec3(0.0F, 0.0F, 1.0F);
  }
  return math::normalize(math::cross(axis, basis));
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
  joint->anchorA =
      to_body_local(transformA, math::sub(pivot, transformA.position));
  joint->anchorB =
      to_body_local(transformB, math::sub(pivot, transformB.position));
  const math::Vec3 worldAxis = math::normalize(axis);
  const math::Vec3 worldRef = perpendicular_reference(worldAxis);
  joint->axis = to_body_local(transformA, worldAxis);
  joint->axisB = to_body_local(transformB, worldAxis);
  joint->twistRefA = to_body_local(transformA, worldRef);
  joint->twistRefB = to_body_local(transformB, worldRef);
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
  joint->anchorA =
      to_body_local(transformA, math::sub(pivot, transformA.position));
  joint->anchorB =
      to_body_local(transformB, math::sub(pivot, transformB.position));
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
  joint->axis = to_body_local(transformA, math::normalize(axis));
  joint->anchorA = math::Vec3(0.0F, 0.0F, 0.0F);
  joint->anchorB = math::Vec3(0.0F, 0.0F, 0.0F);
  joint->referenceRotation = relative_rotation(transformA, transformB);
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
  joint->anchorA = to_body_local(
      transformA, math::sub(transformB.position, transformA.position));
  joint->anchorB = math::Vec3(0.0F, 0.0F, 0.0F);
  joint->referenceRotation = relative_rotation(transformA, transformB);
  joint->accumulatedImpulse = 0.0F;
  return id;
}

/// Sets finite ordered limits on hinge and slider joints. Hinge limits are
/// twist radians restricted to [-pi, pi]: the twist is measured with atan2
/// and therefore wraps, so multi-turn ranges cannot be enforced and are
/// rejected rather than silently clamping at the wrong boundary.
void set_joint_limits(PhysicsWorldView &world, JointId id, float minLimit,
                      float maxLimit) noexcept {
  constexpr float kPi = 3.14159274F;
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
  if ((joint->type == JointType::Hinge) &&
      ((minLimit < -kPi) || (maxLimit > kPi))) {
    core::log_message(core::LogLevel::Error, "physics",
                      "hinge limits must lie within [-pi, pi]");
    return;
  }

  joint->hasLimits = true;
  joint->minLimit = minLimit;
  joint->maxLimit = maxLimit;
}

// --- Main constraint solver -------------------------------------------------

/// Resolves one endpoint's solve frame: a dynamic endpoint (nonzero inverse
/// mass) is a hierarchy root by the World invariant, so its writable local
/// transform is used directly; a static endpoint copies its hierarchy-
/// composed simulation pose into the caller's scratch so a parented anchor
/// is solved where it actually sits and follows parent motion. Static
/// endpoints never receive corrections (zero mass share), so writes to the
/// scratch are inert.
static Transform *resolve_endpoint_pose(
    PhysicsWorldView &world, Entity entity,
    const PhysicsWorldView::SimulationAccessToken &simToken, float invMass,
    Transform *writable, Transform *scratch) noexcept {
  if (invMass > 0.0F) {
    return writable;
  }
  PhysicsTransform composed{};
  if (!world.get_simulation_physics_transform(entity, simToken, &composed)) {
    return nullptr;
  }
  *scratch = Transform{};
  scratch->position = composed.position;
  scratch->rotation = composed.rotation;
  return scratch;
}

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

/// Iteratively solves all active joints for the step: per iteration each
/// joint projects its position constraints and removes violating relative
/// velocity (models and Jacobians are documented per solver file). Warm
/// starting is deliberately limited to the center-line joints (Distance,
/// Spring), which replay 80% of the previous frame's SIGNED impulse along
/// the current center line to speed convergence under persistent load; the
/// multi-DOF joints converge by exact projection, and replaying stale
/// multi-DOF corrections would inject drift at rest that their solvers
/// never remove. A body with zero inverse mass is treated as fully static:
/// its inverse inertia is forced to zero so joint torques cannot spin a
/// static anchor whose RigidBody kept the default inertia. Static
/// endpoints read their hierarchy-COMPOSED simulation pose each iteration
/// so a parented anchor follows its parent; dynamic endpoints are
/// hierarchy roots by the World invariant, so their writable local
/// transform IS their world pose and corrections write through unchanged.
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

    Transform scratchA{};
    Transform scratchB{};
    Transform *poseA = resolve_endpoint_pose(world, j.entityA, simToken,
                                             invMassA, tA, &scratchA);
    Transform *poseB = resolve_endpoint_pose(world, j.entityB, simToken,
                                             invMassB, tB, &scratchB);
    if ((poseA == nullptr) || (poseB == nullptr)) {
      continue;
    }

    const math::Vec3 delta = math::sub(poseB->position, poseA->position);
    const float dist = math::length(delta);
    if (dist < 1e-8F) {
      continue;
    }
    const math::Vec3 dir = math::div(delta, dist);
    const float warmImpulse = j.accumulatedImpulse * 0.8F;
    poseA->position = math::add(
        poseA->position, math::mul(dir, warmImpulse * invMassA / invMassSum));
    poseB->position = math::sub(
        poseB->position, math::mul(dir, warmImpulse * invMassB / invMassSum));
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
      solveCtx.bodyA = bodyA;
      solveCtx.bodyB = bodyB;
      solveCtx.invMassA = (bodyA != nullptr) ? bodyA->inverseMass : 0.0F;
      solveCtx.invMassB = (bodyB != nullptr) ? bodyB->inverseMass : 0.0F;
      solveCtx.invInertiaA = ((bodyA != nullptr) && (bodyA->inverseMass > 0.0F))
                                 ? bodyA->inverseInertia
                                 : 0.0F;
      solveCtx.invInertiaB = ((bodyB != nullptr) && (bodyB->inverseMass > 0.0F))
                                 ? bodyB->inverseInertia
                                 : 0.0F;

      Transform scratchA{};
      Transform scratchB{};
      solveCtx.tA = resolve_endpoint_pose(world, j.entityA, simToken,
                                          solveCtx.invMassA, tA, &scratchA);
      solveCtx.tB = resolve_endpoint_pose(world, j.entityB, simToken,
                                          solveCtx.invMassB, tB, &scratchB);
      if ((solveCtx.tA == nullptr) || (solveCtx.tB == nullptr)) {
        continue;
      }

      const auto jointType = static_cast<JointType>(j.type);

      switch (jointType) {
      case JointType::Distance:
        solve_distance_joint(solveCtx, j);
        break;
      case JointType::Hinge:
        solve_hinge_joint(solveCtx, j);
        break;
      case JointType::BallSocket:
        solve_ball_socket_joint(solveCtx, j);
        break;
      case JointType::Slider:
        solve_slider_joint(solveCtx, j);
        break;
      case JointType::Spring:
        solve_spring_joint(solveCtx, j, deltaSeconds);
        break;
      case JointType::Fixed:
        solve_fixed_joint(solveCtx, j);
        break;
      }
    }
  }
}

} // namespace engine::physics
