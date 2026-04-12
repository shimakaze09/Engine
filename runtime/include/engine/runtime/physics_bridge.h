#pragma once

#include <cstddef>

#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/physics.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

struct PhysicsRaycastHit final {
  Entity entity = kInvalidEntity;
  float distance = 0.0F;
  math::Vec3 point = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 normal = math::Vec3(0.0F, 0.0F, 0.0F);
};

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
void remove_joint(World &world, physics::JointId id) noexcept;

void wake_body(World &world, Entity entity) noexcept;
bool is_sleeping(const World &world, Entity entity) noexcept;

bool set_convex_hull_data(Entity entity,
                          const physics::ConvexHullData &hull) noexcept;
const physics::ConvexHullData *get_convex_hull_data(Entity entity) noexcept;

bool set_heightfield_data(Entity entity,
                          const physics::HeightfieldData &hf) noexcept;
const physics::HeightfieldData *get_heightfield_data(Entity entity) noexcept;

} // namespace engine::runtime