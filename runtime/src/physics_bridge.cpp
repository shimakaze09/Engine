#include "engine/runtime/physics_bridge.h"

#include "engine/core/logging.h"
#include "engine/runtime/world.h"

namespace engine::physics {

bool step_physics(runtime::World &world, float deltaSeconds) noexcept;
bool step_physics_range(runtime::World &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept;
bool resolve_collisions(runtime::World &world) noexcept;
void set_gravity(runtime::World &world, float x, float y, float z) noexcept;
math::Vec3 get_gravity(const runtime::World &world) noexcept;
void set_collision_dispatch(runtime::World &world,
                            CollisionDispatchFn fn) noexcept;
void dispatch_collision_callbacks(runtime::World &world) noexcept;
bool raycast(const runtime::World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             runtime::PhysicsRaycastHit *outHit,
             runtime::Entity skipEntity) noexcept;
JointId add_distance_joint(runtime::World &world, runtime::Entity entityA,
                           runtime::Entity entityB, float distance) noexcept;
void remove_joint(runtime::World &world, JointId id) noexcept;
void wake_body(runtime::World &world, runtime::Entity entity) noexcept;
bool is_sleeping(const runtime::World &world, runtime::Entity entity) noexcept;

} // namespace engine::physics

namespace engine::runtime {

namespace {

bool require_phase(const World &world, WorldPhase phase,
                   const char *apiName) noexcept {
  if (world.current_phase() == phase) {
    return true;
  }

  core::log_message(core::LogLevel::Error, "physics", apiName);
  return false;
}

} // namespace

bool step_physics(World &world, float deltaSeconds) noexcept {
  if (!require_phase(world, WorldPhase::Simulation, "step_physics")) {
    return false;
  }
  return physics::step_physics(world, deltaSeconds);
}

bool step_physics_range(World &world, std::size_t startIndex, std::size_t count,
                        float deltaSeconds) noexcept {
  if (!require_phase(world, WorldPhase::Simulation, "step_physics_range")) {
    return false;
  }
  return physics::step_physics_range(world, startIndex, count, deltaSeconds);
}

bool resolve_collisions(World &world) noexcept {
  if (!require_phase(world, WorldPhase::Simulation, "resolve_collisions")) {
    return false;
  }
  return physics::resolve_collisions(world);
}

void set_gravity(World &world, float x, float y, float z) noexcept {
  if (!require_phase(world, WorldPhase::Input, "set_gravity")) {
    return;
  }
  physics::set_gravity(world, x, y, z);
}

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

void set_collision_dispatch(World &world, physics::CollisionDispatchFn fn) noexcept {
  if (!require_phase(world, WorldPhase::Input, "set_collision_dispatch")) {
    return;
  }
  physics::set_collision_dispatch(world, fn);
}

void dispatch_collision_callbacks(World &world) noexcept {
  if (!require_phase(world, WorldPhase::Input,
                     "dispatch_collision_callbacks")) {
    return;
  }
  physics::dispatch_collision_callbacks(world);
}

bool raycast(const World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit, Entity skipEntity) noexcept {
  return physics::raycast(world, origin, direction, maxDistance, outHit,
                          skipEntity);
}

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

void remove_joint(World &world, physics::JointId id) noexcept {
  if (!require_phase(world, WorldPhase::Input, "remove_joint")) {
    return;
  }
  physics::remove_joint(world, id);
}

void wake_body(World &world, Entity entity) noexcept {
  if (!require_phase(world, WorldPhase::Input, "wake_body")) {
    return;
  }
  physics::wake_body(world, entity);
}

bool is_sleeping(const World &world, Entity entity) noexcept {
  return physics::is_sleeping(world, entity);
}

} // namespace engine::runtime
