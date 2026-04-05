#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::runtime {
class World;
}

namespace engine::scripting {

bool initialize_scripting() noexcept;
void shutdown_scripting() noexcept;
void set_scripting_world(runtime::World *world) noexcept;
void set_default_mesh_asset_id(std::uint32_t assetId) noexcept;

// Set the frame time exposed to Lua via engine.delta_time() / engine.elapsed_time().
// Call once per frame before invoking script callbacks.
void set_frame_time(float deltaSeconds, float totalSeconds) noexcept;

// Load and execute a script file. Returns false and logs on error.
bool load_script(const char *path) noexcept;

// Call a named global function with no args, no return value.
// Returns false if function doesn't exist or errors.
bool call_script_function(const char *name) noexcept;

// Call a named global function with one float argument, no return value.
// Returns false if function doesn't exist or errors.
bool call_script_function_float(const char *name, float arg) noexcept;

// Dispatch Lua on_collision(entityA, entityB) for each pair in pairData.
// pairData is an array of [entityIndexA, entityIndexB, ...] uint32 values.
// pairCount is the number of pairs (not element count).
// No-op if the scripting system is not initialised or on_collision is absent.
void dispatch_physics_callbacks(const std::uint32_t *pairData,
                                 std::size_t pairCount) noexcept;

// Set the current frame index; exposed to Lua via engine.frame_count().
void set_frame_index(std::uint32_t frameIndex) noexcept;

// Begin watching a Lua script file for changes (hot-reload).
void watch_script_file(const char *path) noexcept;

// Check if the watched script file has changed; reload it if so.
void check_script_reload() noexcept;

} // namespace engine::scripting
