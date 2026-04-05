#pragma once

#include <cstddef>

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

namespace engine::physics {

bool step_physics(runtime::World &world, float deltaSeconds) noexcept;
bool step_physics_range(runtime::World &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept;
bool resolve_collisions(runtime::World &world) noexcept;

void set_gravity(float x, float y, float z) noexcept;
math::Vec3 get_gravity() noexcept;

} // namespace engine::physics
