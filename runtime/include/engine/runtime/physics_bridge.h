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

bool step_physics(World &world, float deltaSeconds) noexcept;
bool step_physics_range(World &world, std::size_t startIndex, std::size_t count,
                        float deltaSeconds) noexcept;
bool resolve_collisions(World &world) noexcept;

void set_gravity(World &world, float x, float y, float z) noexcept;
bool get_gravity(const World &world, float *outX, float *outY,
                 float *outZ) noexcept;

void set_collision_dispatch(World &world,
                            physics::CollisionDispatchFn fn) noexcept;
void dispatch_collision_callbacks(World &world) noexcept;

bool raycast(const World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit,
             Entity skipEntity = kInvalidEntity) noexcept;

physics::JointId add_distance_joint(World &world, Entity entityA,
                                    Entity entityB, float distance) noexcept;
physics::JointId add_hinge_joint(World &world, Entity entityA, Entity entityB,
                                 const math::Vec3 &pivot,
                                 const math::Vec3 &axis) noexcept;
physics::JointId add_ball_socket_joint(World &world, Entity entityA,
                                       Entity entityB,
                                       const math::Vec3 &pivot) noexcept;
physics::JointId add_slider_joint(World &world, Entity entityA, Entity entityB,
                                  const math::Vec3 &axis) noexcept;
physics::JointId add_spring_joint(World &world, Entity entityA, Entity entityB,
                                  float restLength, float stiffness,
                                  float damping) noexcept;
physics::JointId add_fixed_joint(World &world, Entity entityA,
                                 Entity entityB) noexcept;
void set_joint_limits(World &world, physics::JointId id, float minLimit,
                      float maxLimit) noexcept;
void remove_joint(World &world, physics::JointId id) noexcept;

void wake_body(World &world, Entity entity) noexcept;
bool is_sleeping(const World &world, Entity entity) noexcept;

bool set_convex_hull_data(Entity entity,
                          const physics::ConvexHullData &hull) noexcept;
const physics::ConvexHullData *get_convex_hull_data(Entity entity) noexcept;

bool set_heightfield_data(Entity entity,
                          const physics::HeightfieldData &hf) noexcept;
const physics::HeightfieldData *get_heightfield_data(Entity entity) noexcept;

// Physics queries (P1-M3-D)
std::size_t raycast_all(const World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        PhysicsRaycastHit *outHits, std::size_t maxHits,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

std::size_t overlap_sphere(const World &world, const math::Vec3 &center,
                           float radius, std::uint32_t *outEntityIndices,
                           std::size_t maxResults,
                           std::uint32_t mask = 0xFFFFFFFFU) noexcept;

std::size_t overlap_box(const World &world, const math::Vec3 &center,
                        const math::Vec3 &halfExtents,
                        std::uint32_t *outEntityIndices, std::size_t maxResults,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

bool sweep_sphere(const World &world, const math::Vec3 &origin, float radius,
                  const math::Vec3 &direction, float maxDistance,
                  physics::SweepHit *outHit,
                  std::uint32_t mask = 0xFFFFFFFFU) noexcept;

bool sweep_box(const World &world, const math::Vec3 &center,
               const math::Vec3 &halfExtents, const math::Vec3 &direction,
               float maxDistance, physics::SweepHit *outHit,
               std::uint32_t mask = 0xFFFFFFFFU) noexcept;

} // namespace engine::runtime