// Implements physics bridge behavior for the Engine runtime world.

#include "engine/runtime/physics_bridge.h"

#include "engine/core/logging.h"
#include "engine/physics/collider.h"
#include "engine/physics/constraint_solver.h"
#include "engine/runtime/world.h"

namespace engine::physics {

/// Handles step physics.
bool step_physics(PhysicsWorldView &world, float deltaSeconds) noexcept;
/// Handles step physics range.
bool step_physics_range(PhysicsWorldView &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept;
/// Handles resolve collisions.
bool resolve_collisions(PhysicsWorldView &world) noexcept;
/// Sets the requested value for gravity.
void set_gravity(PhysicsWorldView &world, float x, float y, float z) noexcept;
/// Returns the requested value for gravity.
math::Vec3 get_gravity(const PhysicsWorldView &world) noexcept;
/// Sets the requested value for collision dispatch.
void set_collision_dispatch(PhysicsWorldView &world,
                            CollisionDispatchFn fn) noexcept;
/// Handles dispatch collision callbacks.
void dispatch_collision_callbacks(PhysicsWorldView &world) noexcept;
/// Handles raycast.
bool raycast(const PhysicsWorldView &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit, Entity skipEntity) noexcept;
/// Adds a value or component to the target system for distance joint.
JointId add_distance_joint(PhysicsWorldView &world, Entity entityA,
                           Entity entityB, float distance) noexcept;
/// Removes a value or component from the target system for joint.
void remove_joint(PhysicsWorldView &world, JointId id) noexcept;
/// Handles wake body.
void wake_body(PhysicsWorldView &world, Entity entity) noexcept;
/// Returns whether is sleeping.
bool is_sleeping(const PhysicsWorldView &world, Entity entity) noexcept;

/// Sets the requested value for convex hull data impl.
bool set_convex_hull_data_impl(std::uint32_t entityIndex,
                               const ConvexHullData &hull) noexcept;
/// Returns the requested value for convex hull data impl.
const ConvexHullData *
get_convex_hull_data_impl(std::uint32_t entityIndex) noexcept;
/// Sets the requested value for heightfield data impl.
bool set_heightfield_data_impl(std::uint32_t entityIndex,
                               const HeightfieldData &hf) noexcept;
/// Returns the requested value for heightfield data impl.
const HeightfieldData *
get_heightfield_data_impl(std::uint32_t entityIndex) noexcept;

} // namespace engine::physics

namespace engine::runtime {

namespace {

/// Handles require phase.
bool require_phase(const World &world, WorldPhase phase,
                   const char *apiName) noexcept {
  if (world.current_phase() == phase) {
    return true;
  }

  // Use apiName as the log channel so the offending call site is visible.
  core::log_message(core::LogLevel::Error, apiName,
                    "called in wrong world phase");
  return false;
}

} // namespace

/// Handles step physics.
bool step_physics(World &world, float deltaSeconds) noexcept {
  if (!require_phase(world, WorldPhase::Simulation, "step_physics")) {
    return false;
  }
  return physics::step_physics(world, deltaSeconds);
}

/// Handles step physics range.
bool step_physics_range(World &world, std::size_t startIndex, std::size_t count,
                        float deltaSeconds) noexcept {
  if (!require_phase(world, WorldPhase::Simulation, "step_physics_range")) {
    return false;
  }
  return physics::step_physics_range(world, startIndex, count, deltaSeconds);
}

/// Handles resolve collisions.
bool resolve_collisions(World &world) noexcept {
  if (!require_phase(world, WorldPhase::Simulation, "resolve_collisions")) {
    return false;
  }
  return physics::resolve_collisions(world);
}

/// Sets the requested value for gravity.
void set_gravity(World &world, float x, float y, float z) noexcept {
  if (!require_phase(world, WorldPhase::Input, "set_gravity")) {
    return;
  }
  physics::set_gravity(world, x, y, z);
}

/// Returns the requested value for gravity.
bool get_gravity(const World &world, float *outX, float *outY,
                 float *outZ) noexcept {
  if ((outX == nullptr) || (outY == nullptr) || (outZ == nullptr)) {
    return false;
  }

  const math::Vec3 gravity = physics::get_gravity(world);
  *outX = gravity.x;
  *outY = gravity.y;
  *outZ = gravity.z;
  return true;
}

/// Sets the requested value for collision dispatch.
void set_collision_dispatch(World &world,
                            physics::CollisionDispatchFn fn) noexcept {
  if (!require_phase(world, WorldPhase::Input, "set_collision_dispatch")) {
    return;
  }
  physics::set_collision_dispatch(world, fn);
}

/// Handles dispatch collision callbacks.
void dispatch_collision_callbacks(World &world) noexcept {
  if (!require_phase(world, WorldPhase::Input,
                     "dispatch_collision_callbacks")) {
    return;
  }
  physics::dispatch_collision_callbacks(world);
}

/// Handles raycast.
bool raycast(const World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit, Entity skipEntity) noexcept {
  return physics::raycast(world, origin, direction, maxDistance, outHit,
                          skipEntity);
}

/// Adds a value or component to the target system for distance joint.
physics::JointId add_distance_joint(World &world, Entity entityA,
                                    Entity entityB, float distance) noexcept {
  if (!require_phase(world, WorldPhase::Input, "add_distance_joint")) {
    return physics::kInvalidJointId;
  }

  if ((entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      !world.is_alive(entityA) || !world.is_alive(entityB)) {
    return physics::kInvalidJointId;
  }

  return physics::add_distance_joint(world, entityA, entityB, distance);
}

/// Adds a value or component to the target system for hinge joint.
physics::JointId add_hinge_joint(World &world, Entity entityA, Entity entityB,
                                 const math::Vec3 &pivot,
                                 const math::Vec3 &axis) noexcept {
  if (!require_phase(world, WorldPhase::Input, "add_hinge_joint")) {
    return physics::kInvalidJointId;
  }
  if ((entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      !world.is_alive(entityA) || !world.is_alive(entityB)) {
    return physics::kInvalidJointId;
  }
  return physics::add_hinge_joint(world, entityA, entityB, pivot, axis);
}

/// Adds a value or component to the target system for ball socket joint.
physics::JointId add_ball_socket_joint(World &world, Entity entityA,
                                       Entity entityB,
                                       const math::Vec3 &pivot) noexcept {
  if (!require_phase(world, WorldPhase::Input, "add_ball_socket_joint")) {
    return physics::kInvalidJointId;
  }
  if ((entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      !world.is_alive(entityA) || !world.is_alive(entityB)) {
    return physics::kInvalidJointId;
  }
  return physics::add_ball_socket_joint(world, entityA, entityB, pivot);
}

/// Adds a value or component to the target system for slider joint.
physics::JointId add_slider_joint(World &world, Entity entityA, Entity entityB,
                                  const math::Vec3 &axis) noexcept {
  if (!require_phase(world, WorldPhase::Input, "add_slider_joint")) {
    return physics::kInvalidJointId;
  }
  if ((entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      !world.is_alive(entityA) || !world.is_alive(entityB)) {
    return physics::kInvalidJointId;
  }
  return physics::add_slider_joint(world, entityA, entityB, axis);
}

/// Adds a value or component to the target system for spring joint.
physics::JointId add_spring_joint(World &world, Entity entityA, Entity entityB,
                                  float restLength, float stiffness,
                                  float damping) noexcept {
  if (!require_phase(world, WorldPhase::Input, "add_spring_joint")) {
    return physics::kInvalidJointId;
  }
  if ((entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      !world.is_alive(entityA) || !world.is_alive(entityB)) {
    return physics::kInvalidJointId;
  }
  return physics::add_spring_joint(world, entityA, entityB, restLength,
                                   stiffness, damping);
}

/// Adds a value or component to the target system for fixed joint.
physics::JointId add_fixed_joint(World &world, Entity entityA,
                                 Entity entityB) noexcept {
  if (!require_phase(world, WorldPhase::Input, "add_fixed_joint")) {
    return physics::kInvalidJointId;
  }
  if ((entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      !world.is_alive(entityA) || !world.is_alive(entityB)) {
    return physics::kInvalidJointId;
  }
  return physics::add_fixed_joint(world, entityA, entityB);
}

/// Sets the requested value for joint limits.
void set_joint_limits(World &world, physics::JointId id, float minLimit,
                      float maxLimit) noexcept {
  if (!require_phase(world, WorldPhase::Input, "set_joint_limits")) {
    return;
  }
  physics::set_joint_limits(world, id, minLimit, maxLimit);
}

/// Removes a value or component from the target system for joint.
void remove_joint(World &world, physics::JointId id) noexcept {
  if (!require_phase(world, WorldPhase::Input, "remove_joint")) {
    return;
  }
  physics::remove_joint(world, id);
}

/// Handles wake body.
void wake_body(World &world, Entity entity) noexcept {
  if (!require_phase(world, WorldPhase::Input, "wake_body")) {
    return;
  }
  physics::wake_body(world, entity);
}

/// Returns whether is sleeping.
bool is_sleeping(const World &world, Entity entity) noexcept {
  return physics::is_sleeping(world, entity);
}

/// Sets the requested value for convex hull data.
bool set_convex_hull_data(Entity entity,
                          const physics::ConvexHullData &hull) noexcept {
  return physics::set_convex_hull_data_impl(entity.index, hull);
}

/// Returns the requested value for convex hull data.
const physics::ConvexHullData *get_convex_hull_data(Entity entity) noexcept {
  return physics::get_convex_hull_data_impl(entity.index);
}

/// Sets the requested value for heightfield data.
bool set_heightfield_data(Entity entity,
                          const physics::HeightfieldData &hf) noexcept {
  return physics::set_heightfield_data_impl(entity.index, hf);
}

/// Returns the requested value for heightfield data.
const physics::HeightfieldData *get_heightfield_data(Entity entity) noexcept {
  return physics::get_heightfield_data_impl(entity.index);
}

// ------ Physics Queries (P1-M3-D) -------------------------------------------

std::size_t raycast_all(const World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        PhysicsRaycastHit *outHits, std::size_t maxHits,
                        std::uint32_t mask) noexcept {
  return physics::raycast_all(world, origin, direction, maxDistance, outHits,
                              maxHits, mask);
}

/// Handles overlap sphere.
std::size_t overlap_sphere(const World &world, const math::Vec3 &center,
                           float radius, std::uint32_t *outEntityIndices,
                           std::size_t maxResults,
                           std::uint32_t mask) noexcept {
  return physics::overlap_sphere(world, center, radius, outEntityIndices,
                                 maxResults, mask);
}

/// Handles overlap box.
std::size_t overlap_box(const World &world, const math::Vec3 &center,
                        const math::Vec3 &halfExtents,
                        std::uint32_t *outEntityIndices, std::size_t maxResults,
                        std::uint32_t mask) noexcept {
  return physics::overlap_box(world, center, halfExtents, outEntityIndices,
                              maxResults, mask);
}

/// Handles sweep sphere.
bool sweep_sphere(const World &world, const math::Vec3 &origin, float radius,
                  const math::Vec3 &direction, float maxDistance,
                  physics::SweepHit *outHit, std::uint32_t mask) noexcept {
  return physics::sweep_sphere(world, origin, radius, direction, maxDistance,
                               outHit, mask);
}

/// Handles sweep box.
bool sweep_box(const World &world, const math::Vec3 &center,
               const math::Vec3 &halfExtents, const math::Vec3 &direction,
               float maxDistance, physics::SweepHit *outHit,
               std::uint32_t mask) noexcept {
  return physics::sweep_box(world, center, halfExtents, direction, maxDistance,
                            outHit, mask);
}

} // namespace engine::runtime
