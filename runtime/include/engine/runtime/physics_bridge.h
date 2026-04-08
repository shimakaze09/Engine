#pragma once

#include <cstddef>

#include "engine/math/vec3.h"
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

bool raycast(const World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit,
             Entity skipEntity = kInvalidEntity) noexcept;

physics::JointId add_distance_joint(World &world, Entity entityA,
                                    Entity entityB, float distance) noexcept;
void remove_joint(physics::JointId id) noexcept;

void wake_body(World &world, Entity entity) noexcept;
bool is_sleeping(const World &world, Entity entity) noexcept;

} // namespace engine::runtime