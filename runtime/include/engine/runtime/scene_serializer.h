#pragma once

#include <cstddef>

namespace engine::runtime {

class World;

bool save_scene(const World &world, const char *path) noexcept;
bool save_scene(const World &world, char *buffer, std::size_t capacity,
                std::size_t *outSize) noexcept;
bool load_scene(World &world, const char *path) noexcept;
bool load_scene(World &world, const char *buffer, std::size_t size) noexcept;
void reset_world(World &world) noexcept;

} // namespace engine::runtime
