#pragma once

#include <cstddef>

#include "engine/runtime/world.h"

namespace engine::physics {

bool step_physics(runtime::World &world, float deltaSeconds) noexcept;
bool step_physics_range(runtime::World &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept;
bool resolve_collisions(runtime::World &world) noexcept;

} // namespace engine::physics
