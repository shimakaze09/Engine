// Declares physics bridge types and APIs for the Engine runtime world.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_query.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

// PhysicsRaycastHit is defined in engine::physics (physics_types.h),
// re-exported here for runtime callers.
using engine::physics::PhysicsRaycastHit;

/// Handles step physics.
bool step_physics(World &world, float deltaSeconds) noexcept;
/// Handles step physics range.
bool step_physics_range(World &world, std::size_t startIndex, std::size_t count,
                        float deltaSeconds) noexcept;
/// Handles resolve collisions.
bool resolve_collisions(World &world,
                        float deltaSeconds = 1.0F / 60.0F) noexcept;

/// Sets the requested value for gravity.
void set_gravity(World &world, float x, float y, float z) noexcept;
/// Returns the requested value for gravity.
bool get_gravity(const World &world, float *outX, float *outY,
                 float *outZ) noexcept;

/// Sets the requested value for collision dispatch.
void set_collision_dispatch(World &world,
                            physics::CollisionDispatchFn fn) noexcept;
/// Handles dispatch collision callbacks.
void dispatch_collision_callbacks(World &world) noexcept;

/// Handles raycast.
bool raycast(const World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit,
             Entity skipEntity = kInvalidEntity) noexcept;

/// Adds a value or component to the target system for distance joint.
physics::JointId add_distance_joint(World &world, Entity entityA,
                                    Entity entityB, float distance) noexcept;
/// Adds a value or component to the target system for hinge joint.
physics::JointId add_hinge_joint(World &world, Entity entityA, Entity entityB,
                                 const math::Vec3 &pivot,
                                 const math::Vec3 &axis) noexcept;
/// Adds a value or component to the target system for ball socket joint.
physics::JointId add_ball_socket_joint(World &world, Entity entityA,
                                       Entity entityB,
                                       const math::Vec3 &pivot) noexcept;
/// Adds a value or component to the target system for slider joint.
physics::JointId add_slider_joint(World &world, Entity entityA, Entity entityB,
                                  const math::Vec3 &axis) noexcept;
/// Adds a value or component to the target system for spring joint.
physics::JointId add_spring_joint(World &world, Entity entityA, Entity entityB,
                                  float restLength, float stiffness,
                                  float damping) noexcept;
/// Adds a value or component to the target system for fixed joint.
physics::JointId add_fixed_joint(World &world, Entity entityA,
                                 Entity entityB) noexcept;
/// Sets the requested value for joint limits.
void set_joint_limits(World &world, physics::JointId id, float minLimit,
                      float maxLimit) noexcept;
/// Removes a value or component from the target system for joint.
void remove_joint(World &world, physics::JointId id) noexcept;

/// Handles wake body.
void wake_body(World &world, Entity entity) noexcept;
/// Returns whether is sleeping.
bool is_sleeping(const World &world, Entity entity) noexcept;

/// Sets the requested value for convex hull data.
bool set_convex_hull_data(World &world, Entity entity,
                          const physics::ConvexHullData &hull) noexcept;
/// Returns the requested value for convex hull data.
const physics::ConvexHullData *get_convex_hull_data(
    const World &world, Entity entity) noexcept;

/// Sets the requested value for heightfield data.
bool set_heightfield_data(World &world, Entity entity,
                          const physics::HeightfieldData &hf) noexcept;
/// Returns the requested value for heightfield data.
const physics::HeightfieldData *get_heightfield_data(
    const World &world, Entity entity) noexcept;

// Physics queries (P1-M3-D)
std::size_t raycast_all(const World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        PhysicsRaycastHit *outHits, std::size_t maxHits,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Handles overlap sphere.
std::size_t overlap_sphere(const World &world, const math::Vec3 &center,
                           float radius, std::uint32_t *outEntityIndices,
                           std::size_t maxResults,
                           std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Handles overlap box.
std::size_t overlap_box(const World &world, const math::Vec3 &center,
                        const math::Vec3 &halfExtents,
                        std::uint32_t *outEntityIndices, std::size_t maxResults,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Handles sweep sphere.
bool sweep_sphere(const World &world, const math::Vec3 &origin, float radius,
                  const math::Vec3 &direction, float maxDistance,
                  physics::SweepHit *outHit,
                  std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Handles sweep box.
bool sweep_box(const World &world, const math::Vec3 &center,
               const math::Vec3 &halfExtents, const math::Vec3 &direction,
               float maxDistance, physics::SweepHit *outHit,
               std::uint32_t mask = 0xFFFFFFFFU) noexcept;

} // namespace engine::runtime
