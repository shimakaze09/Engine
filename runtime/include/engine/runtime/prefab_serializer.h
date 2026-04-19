#pragma once

#include "engine/core/entity.h"

namespace engine::runtime {

using engine::core::Entity;

class World;

// Save a single entity and all its components to a JSON prefab file.
// Returns false and logs on error.
bool save_prefab(const World &world, Entity entity, const char *path) noexcept;

// Instantiate a new entity from a JSON prefab file and add it to the world.
// Returns the new entity; returns kInvalidEntity on error.
Entity instantiate_prefab(World &world, const char *path) noexcept;

} // namespace engine::runtime
