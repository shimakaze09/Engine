// Declares scripting types and APIs for the Engine Lua scripting system.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/entity.h"

namespace engine::runtime {
class World;
} // namespace engine::runtime

namespace engine::scripting {

/// Function-pointer bridge the runtime installs so the animation Lua
/// bindings reach the animation system without an upward link dependency
/// (scripting never links engine_runtime).
struct AnimationScriptBridge final {
  bool (*queueParam)(core::Entity entity, const char *name,
                     float value) noexcept = nullptr;
  std::size_t (*firedEventCount)() noexcept = nullptr;
  bool (*firedEventAt)(std::size_t index, core::Entity *outEntity,
                       const char **outName) noexcept = nullptr;
};

/// Installs (or, with a default-constructed bridge, clears) the animation
/// bridge the Lua animation bindings call through.
void set_animation_script_bridge(const AnimationScriptBridge &bridge) noexcept;

/// Initializes the owning system for scripting.
bool initialize_scripting() noexcept;
/// Shuts down the owning system for scripting.
void shutdown_scripting() noexcept;
/// Sets the requested value for default mesh asset id.
void set_default_mesh_asset_id(std::uint64_t assetId) noexcept;

// Set AssetIds for built-in procedural shape meshes.  Any id equal to 0 means
// that shape is unavailable and spawn_shape("name",...) will fall back to the
// default mesh.
void set_builtin_mesh_ids(std::uint64_t planeMesh, std::uint64_t cubeMesh,
                          std::uint64_t sphereMesh, std::uint64_t cylinderMesh,
                          std::uint64_t capsuleMesh,
                          std::uint64_t pyramidMesh) noexcept;

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

// Debugger breakpoint controls (used by DAP transport).
void debugger_clear_breakpoints() noexcept;
/// Clears only one source file's breakpoints (DAP per-source replace).
void debugger_clear_breakpoints_for_source(const char *file) noexcept;
/// Adds a DAP breakpoint; false when the breakpoint table is full.
bool debugger_add_breakpoint(const char *file, int line) noexcept;

// Dispatch Lua on_collision(entityA, entityB) for each pair in pairData.
// pairData is an array of [entityIndexA, entityIndexB, ...] uint32 values.
// pairCount is the number of pairs (not element count).
// No-op if the scripting system is not initialised or no handlers are present.
void dispatch_physics_callbacks(const std::uint32_t *pairData,
                                std::size_t pairCount) noexcept;

// Dispatch registered Lua handlers and the global on_anim_event fallback
// for every animation event fired by the last fixed-step animation update.
void dispatch_animation_event_callbacks() noexcept;

// Set the current frame index; exposed to Lua via engine.frame_count().
void set_frame_index(std::uint32_t frameIndex) noexcept;

// Tick all active timers; call once per frame before on_update.
void tick_timers() noexcept;

// Tick all active coroutines; call once per frame before on_update.
void tick_coroutines() noexcept;

// Apply queued script-side world mutations. Call once per frame while the
// world is in Input phase.
void flush_deferred_mutations() noexcept;

// Clear all active coroutines (called on stop/reload).
void clear_coroutines() noexcept;

// Release Lua timer callback registry refs and clear the bound world's
// timers (called on stop/reload and on every scene transition so no timer
// closure outlives the world it was scheduled against).
void clear_timers() noexcept;

// Retire every Lua-created entity pool (called on stop/reload and on every
// scene transition so pool slots don't leak and stale pool ids can't alias
// a replacement-world pool of the same slot number).
void clear_entity_pools() noexcept;

// Introspection: live Lua timer-callback registry refs / allocated entity
// pool slots. Exercised by scene-transition regression tests; a correct
// transition drains both to zero.
std::size_t active_timer_ref_count() noexcept;
std::size_t active_entity_pool_count() noexcept;

// Scene operation query — engine.cpp polls these after each fixed-step batch.
bool has_pending_scene_op() noexcept;
/// True when the pending op is a scene load.
bool pending_scene_op_is_load() noexcept;
/// True when the pending op is a new-scene request.
bool pending_scene_op_is_new() noexcept;
/// Path argument of the pending scene op ("" when none).
const char *get_pending_scene_path() noexcept;
/// Drops the pending scene op without applying it.
void clear_pending_scene_op() noexcept;

// Begin watching a Lua script file for changes (hot-reload).
void watch_script_file(const char *path) noexcept;
/// Count of scripts currently in the hot-reload watch table; unchanged by
/// a rejected (over-long or jailed) watch_script_file call.
std::size_t watched_script_count() noexcept;

// Reload changed watched scripts atomically; failed execution restores all
// previous top-level global bindings.
void check_script_reload() noexcept;

// --- Per-entity script dispatch (ScriptComponent) ---
// Each entity with a ScriptComponent references a Lua script file that returns
// a module table. The canonical hooks are on_begin_play(self), on_tick(self,
// dt), on_end_play(self), on_save_state(self), and on_reload(self, state);
// legacy on_start/on_update/on_end names remain fallbacks. `self` is an opaque,
// generation-checked handle. Multiple entities may share the same script file.
//
// on_tick cadence (audit #176, corrects the prior "once per simulation step"
// claim to match the always-intentional EnginePipeline::stage_scripting
// behavior, audit M-01): on_tick is a per-rendered-frame callback, not a
// per-fixed-step one. It fires exactly once per frame that advanced
// simulation, with dt equal to the total time simulated that frame — the sum
// of every catch-up fixed step folded into it, not one call per step. A
// frame that runs three fixed steps calls on_tick once with dt = 3 *
// kFixedDeltaSeconds, the same way physics and animation each ran three
// times that frame but transform propagation only publishes once. Re-entrant
// per-step dispatch would multiply gameplay callbacks and their deferred
// mutations; passing the bare fixed delta instead of the summed time made
// timers and script-driven motion run slow under catch-up. There is
// currently no separate per-fixed-step Lua callback — only on_tick.

// Load all unique script files referenced by ScriptComponents in the world and
// call module.on_begin_play(self) for each entity. Call once on Play start.
void dispatch_entity_scripts_start() noexcept;

// Dispatch on_begin_play for entities that need it (newly created).
// Marks begin_play done on delivery; a failed module load leaves the
// entity pending and retries under the mtime-gated attempt budget.
void dispatch_entity_scripts_begin_play(runtime::World *world) noexcept;

// Dispatch on_end_play(self) for entities pending deferred destruction.
void dispatch_entity_scripts_end_play(runtime::World *world) noexcept;

// Restore pending reload state, then call module.on_tick(self, dt) for every
// entity with a ScriptComponent. Call once per rendered frame that advanced
// simulation (not once per fixed step); dt is the frame's total simulated
// time — see the on_tick cadence note above.
void dispatch_entity_scripts_update(float dt) noexcept;

// Call module.on_end_play(self) for every entity with a ScriptComponent.
// Call once when Play transitions to Stopped.
void dispatch_entity_scripts_end() noexcept;

// Call module.on_end_play(self) for every entity with a ScriptComponent in
// the outgoing world, immediately before a script-driven scene transition
// (engine.load_scene/engine.new_scene) commits its replacement content
// (#198); same dispatch as dispatch_entity_scripts_end() but additionally
// rejects a handler's own load_scene/new_scene call and defers rather than
// applies any world mutation the handler triggers, so a reentrant handler
// cannot corrupt the transition already in flight. Call once from
// process_pending_scene_op, before the outgoing world's content is
// replaced or cleared.
void dispatch_entity_scripts_end_for_transition() noexcept;

// Drop all cached entity script modules and unclaimed reload state.
void clear_entity_script_modules() noexcept;

// --- Sandbox configuration ---
// Enable or disable the Lua sandbox (restricted globals, CPU/memory limits).
void set_sandbox_enabled(bool enabled) noexcept;
/// Returns whether is sandbox enabled.
bool is_sandbox_enabled() noexcept;

// CPU instruction budget per frame, shared across all dispatches,
// coroutines, and hooks; refilled at set_frame_index (0 = unlimited).
void set_instruction_limit(int limit) noexcept;
/// Current per-frame shared Lua instruction cap (0 = unlimited).
int get_instruction_limit() noexcept;

// Memory limit for the Lua allocator in bytes (0 = unlimited).
void set_memory_limit(std::size_t limit) noexcept;
/// Current Lua allocator byte cap (0 = unlimited).
std::size_t get_memory_limit() noexcept;
/// Bytes currently allocated by the scripting VM (accounted from state
/// creation; reset by shutdown_scripting).
std::size_t get_memory_used() noexcept;

} // namespace engine::scripting
