#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::scripting {

bool initialize_scripting() noexcept;
void shutdown_scripting() noexcept;
void set_default_mesh_asset_id(std::uint32_t assetId) noexcept;

// Set AssetIds for built-in procedural shape meshes.  Any id equal to 0 means
// that shape is unavailable and spawn_shape("name",...) will fall back to the
// default mesh.
void set_builtin_mesh_ids(std::uint32_t planeMesh, std::uint32_t cubeMesh,
                          std::uint32_t sphereMesh, std::uint32_t cylinderMesh,
                          std::uint32_t capsuleMesh,
                          std::uint32_t pyramidMesh) noexcept;

// Set the frame time exposed to Lua via engine.delta_time() /
// engine.elapsed_time(). Call once per frame before invoking script callbacks.
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
// No-op if the scripting system is not initialised or no handlers are present.
void dispatch_physics_callbacks(const std::uint32_t *pairData,
                                std::size_t pairCount) noexcept;

// Set the current frame index; exposed to Lua via engine.frame_count().
void set_frame_index(std::uint32_t frameIndex) noexcept;

// Tick all active timers; call once per frame before on_update.
void tick_timers() noexcept;

// Tick all active coroutines; call once per frame before on_update.
void tick_coroutines() noexcept;

// Clear all active coroutines (called on stop/reload).
void clear_coroutines() noexcept;

// Scene operation query — engine.cpp polls these after each fixed-step batch.
bool has_pending_scene_op() noexcept;
bool pending_scene_op_is_load() noexcept;
bool pending_scene_op_is_new() noexcept;
const char *get_pending_scene_path() noexcept;
void clear_pending_scene_op() noexcept;

// Begin watching a Lua script file for changes (hot-reload).
void watch_script_file(const char *path) noexcept;

// Check if the watched script file has changed; reload it if so.
void check_script_reload() noexcept;

// --- Per-entity script dispatch (ScriptComponent) ---
// Each entity with a ScriptComponent references a Lua script file that returns
// a module table with optional on_start(self) and on_update(self, dt) entries.
// Multiple entities may share the same script file.

// Load all unique script files referenced by ScriptComponents in the world and
// call module.on_start(entityIndex) for each entity. Call once on Play start.
void dispatch_entity_scripts_start() noexcept;

// Call module.on_update(entityIndex, dt) for every entity with a
// ScriptComponent. Call once per simulation step.
void dispatch_entity_scripts_update(float dt) noexcept;

// Drop all cached entity script modules (called on Stop / reload).
void clear_entity_script_modules() noexcept;

} // namespace engine::scripting
