#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::runtime {
class World;
}

namespace engine::scripting {

struct RaycastHit final {
  std::uint32_t entityIndex = 0U;
  float distance = 0.0F;
  float pointX = 0.0F;
  float pointY = 0.0F;
  float pointZ = 0.0F;
  float normalX = 0.0F;
  float normalY = 0.0F;
  float normalZ = 0.0F;
};

struct Services final {
  void (*set_camera_position)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_target)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_up)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_fov)(float fovRadians) noexcept = nullptr;

  void (*set_gravity)(float x, float y, float z) noexcept = nullptr;
  bool (*get_gravity)(float *outX, float *outY, float *outZ) noexcept = nullptr;
  bool (*raycast)(runtime::World *world, float ox, float oy, float oz, float dx,
                  float dy, float dz, float maxDistance,
                  RaycastHit *outHit) noexcept = nullptr;
  std::uint32_t (*add_distance_joint)(runtime::World *world,
                                      std::uint32_t entityIndexA,
                                      std::uint32_t entityIndexB,
                                      float distance) noexcept = nullptr;
  void (*remove_joint)(std::uint32_t jointId) noexcept = nullptr;
  void (*wake_body)(runtime::World *world,
                    std::uint32_t entityIndex) noexcept = nullptr;
  bool (*is_sleeping)(runtime::World *world,
                      std::uint32_t entityIndex) noexcept = nullptr;

  std::uint32_t (*load_sound)(const char *path) noexcept = nullptr;
  void (*unload_sound)(std::uint32_t soundId) noexcept = nullptr;
  bool (*play_sound)(std::uint32_t soundId, float volume, float pitch,
                     bool loop) noexcept = nullptr;
  void (*stop_sound)(std::uint32_t soundId) noexcept = nullptr;
  void (*stop_all_sounds)() noexcept = nullptr;
  void (*set_master_volume)(float volume) noexcept = nullptr;

  bool (*save_scene)(const runtime::World *world,
                     const char *path) noexcept = nullptr;
  bool (*save_prefab)(const runtime::World *world, std::uint32_t entityIndex,
                      const char *path) noexcept = nullptr;
  std::uint32_t (*instantiate_prefab)(runtime::World *world,
                                      const char *path) noexcept = nullptr;
};

bool initialize_scripting() noexcept;
void shutdown_scripting() noexcept;
void set_scripting_world(runtime::World *world) noexcept;
void set_services(const Services *services) noexcept;
void set_default_mesh_asset_id(std::uint32_t assetId) noexcept;

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
