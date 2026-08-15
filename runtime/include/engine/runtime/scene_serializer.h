// Declares scene serializer types and APIs for the Engine runtime world.

#pragma once

#include <cstddef>

namespace engine::runtime {

class World;

/// Callback invoked immediately before a scene transition destructively
/// replaces (load_scene) or clears (reset_world) the live world's content,
/// while the outgoing entities and their script modules are still alive
/// and callable; used by process_pending_scene_op to dispatch on_end_play
/// to the outgoing scene before teardown (#198). Never called on a failed
/// load — a failure leaves the outgoing world untouched and unreported.
using SceneTeardownHook = void (*)() noexcept;

/// Saves the requested resource for scene.
bool save_scene(const World &world, const char *path) noexcept;
/// Saves the requested resource for scene.
bool save_scene(const World &world, char *buffer, std::size_t capacity,
                std::size_t *outSize) noexcept;
/// Loads the requested resource for scene.
bool load_scene(World &world, const char *path,
                SceneTeardownHook beforeTeardown = nullptr) noexcept;
/// Loads the requested resource for scene.
bool load_scene(World &world, const char *buffer, std::size_t size,
                SceneTeardownHook beforeTeardown = nullptr) noexcept;
/// Resets this object back to its reusable empty state for world.
void reset_world(World &world,
                 SceneTeardownHook beforeTeardown = nullptr) noexcept;

} // namespace engine::runtime
