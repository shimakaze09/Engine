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

/// Integrates all rigid bodies for one fixed step (full range).
bool step_physics(World &world, float deltaSeconds) noexcept;
/// Integrates rigid bodies in one dense-index chunk; parallel-safe with
/// disjoint ranges.
bool step_physics_range(World &world, std::size_t startIndex, std::size_t count,
                        float deltaSeconds) noexcept;
/// Broadphase + narrow phase + constraint solve + sleep pass.
bool resolve_collisions(World &world,
                        float deltaSeconds = 1.0F / 60.0F) noexcept;

/// Sets the requested value for gravity.
void set_gravity(World &world, float x, float y, float z) noexcept;
/// Reads the world's gravity vector; false on null outputs.
bool get_gravity(const World &world, float *outX, float *outY,
                 float *outZ) noexcept;

/// Sets the requested value for collision dispatch.
void set_collision_dispatch(World &world,
                            physics::CollisionDispatchFn fn) noexcept;
/// Forwards this step's collision pairs to the registered dispatch.
void dispatch_collision_callbacks(World &world) noexcept;

/// Closest-hit raycast; false when nothing is hit within maxDistance.
bool raycast(const World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit,
             Entity skipEntity = kInvalidEntity) noexcept;

/// Creates a distance joint holding the entities `distance` apart;
/// kInvalidJointId on failure.
physics::JointId add_distance_joint(World &world, Entity entityA,
                                    Entity entityB, float distance) noexcept;
/// Creates a hinge joint about `axis` at `pivot`; kInvalidJointId on failure.
physics::JointId add_hinge_joint(World &world, Entity entityA, Entity entityB,
                                 const math::Vec3 &pivot,
                                 const math::Vec3 &axis) noexcept;
/// Creates a ball-socket joint at `pivot`; kInvalidJointId on failure.
physics::JointId add_ball_socket_joint(World &world, Entity entityA,
                                       Entity entityB,
                                       const math::Vec3 &pivot) noexcept;
/// Creates a slider joint along `axis`; kInvalidJointId on failure.
physics::JointId add_slider_joint(World &world, Entity entityA, Entity entityB,
                                  const math::Vec3 &axis) noexcept;
/// Creates a spring joint (rest length, stiffness, damping);
/// kInvalidJointId on failure.
physics::JointId add_spring_joint(World &world, Entity entityA, Entity entityB,
                                  float restLength, float stiffness,
                                  float damping) noexcept;
/// Creates a fixed joint locking both bodies; kInvalidJointId on failure.
physics::JointId add_fixed_joint(World &world, Entity entityA,
                                 Entity entityB) noexcept;
/// Sets the requested value for joint limits.
void set_joint_limits(World &world, physics::JointId id, float minLimit,
                      float maxLimit) noexcept;
/// Releases the joint slot; safe with kInvalidJointId.
void remove_joint(World &world, physics::JointId id) noexcept;

/// Clears the entity's sleep state so simulation resumes.
void wake_body(World &world, Entity entity) noexcept;
/// Returns whether is sleeping.
bool is_sleeping(const World &world, Entity entity) noexcept;

/// Sets the requested value for convex hull data.
bool set_convex_hull_data(World &world, Entity entity,
                          const physics::ConvexHullData &hull) noexcept;
/// Entity's convex-hull payload, or nullptr when none is set.
const physics::ConvexHullData *get_convex_hull_data(
    const World &world, Entity entity) noexcept;

/// Sets the requested value for heightfield data.
bool set_heightfield_data(World &world, Entity entity,
                          const physics::HeightfieldData &hf) noexcept;
/// Entity's heightfield payload, or nullptr when none is set.
const physics::HeightfieldData *get_heightfield_data(
    const World &world, Entity entity) noexcept;

// Physics queries (P1-M3-D)
std::size_t raycast_all(const World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        PhysicsRaycastHit *outHits, std::size_t maxHits,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Collects entity indices overlapping the sphere (mask-filtered);
/// returns the hit count.
std::size_t overlap_sphere(const World &world, const math::Vec3 &center,
                           float radius, std::uint32_t *outEntityIndices,
                           std::size_t maxResults,
                           std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Collects entity indices overlapping the AABB (mask-filtered);
/// returns the hit count.
std::size_t overlap_box(const World &world, const math::Vec3 &center,
                        const math::Vec3 &halfExtents,
                        std::uint32_t *outEntityIndices, std::size_t maxResults,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Sweeps a sphere along direction; earliest hit within maxDistance.
bool sweep_sphere(const World &world, const math::Vec3 &origin, float radius,
                  const math::Vec3 &direction, float maxDistance,
                  physics::SweepHit *outHit,
                  std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Sweeps an AABB along direction; earliest hit within maxDistance.
bool sweep_box(const World &world, const math::Vec3 &center,
               const math::Vec3 &halfExtents, const math::Vec3 &direction,
               float maxDistance, physics::SweepHit *outHit,
               std::uint32_t mask = 0xFFFFFFFFU) noexcept;

} // namespace engine::runtime
