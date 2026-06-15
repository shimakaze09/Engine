// Declares scene serializer types and APIs for the Engine runtime world.

#pragma once

#include <cstddef>

namespace engine::runtime {

/// Owns the world behavior and state.
class World;

/// Saves the requested resource for scene.
bool save_scene(const World &world, const char *path) noexcept;
/// Saves the requested resource for scene.
bool save_scene(const World &world, char *buffer, std::size_t capacity,
                std::size_t *outSize) noexcept;
/// Loads the requested resource for scene.
bool load_scene(World &world, const char *path) noexcept;
/// Loads the requested resource for scene.
bool load_scene(World &world, const char *buffer, std::size_t size) noexcept;
/// Resets this object back to its reusable empty state for world.
void reset_world(World &world) noexcept;

} // namespace engine::runtime
