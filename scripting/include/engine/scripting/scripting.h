#pragma once

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

} // namespace engine::scripting
