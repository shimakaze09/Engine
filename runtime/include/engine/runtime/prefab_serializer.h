// Declares prefab serializer types and APIs for the Engine runtime world.

#pragma once

#include "engine/core/entity.h"

namespace engine::runtime {

using engine::core::Entity;

class World;

// Save a single entity and all its components to a JSON prefab file, one
// field per line as a scene file is. A
// path under a mounted virtual prefix names the mounted file wherever the
// process was started; any other path is an OS path. An entity with
// children is refused, since a prefab holds a single entity. Returns false
// and logs on error.
bool save_prefab(const World &world, Entity entity, const char *path) noexcept;

// Instantiate a new entity from a JSON prefab file (its path resolved as
// save_prefab resolves it) and add it to the world. A key no reader looks
// up is logged as a Warning naming its path, and the instance is still
// made. Returns the new entity; returns kInvalidEntity on error.
Entity instantiate_prefab(World &world, const char *path) noexcept;

} // namespace engine::runtime
