// Implements scripting behavior for the Engine Lua scripting system.

#include "engine/scripting/scripting.h"
#include "animation_bindings.h"
#include "asset_bindings.h"
#include "audio_bindings.h"
#include "binding_util.h"
#include "body_bindings.h"
#include "camera_bindings.h"
#include "cheat_bindings.h"
#include "collision_bindings.h"
#include "coroutine_bindings.h"
#include "debug_bindings.h"
#include "deferred_mutations.h"
#include "deterministic_math_library.h"
#include "engine/scripting/bindable_api.h"
#include "engine/scripting/dap_server.h"
#include "entity_handle.h"
#include "entity_lifecycle_bindings.h"
#include "entity_pool_bindings.h"
#include "entity_script_bindings.h"
#include "game_bindings.h"
#include "input_bindings.h"
#include "light_bindings.h"
#include "lua_state.h"
#include "mesh_material_bindings.h"
#include "persist_bindings.h"
#include "physics_bindings.h"
#include "random_bindings.h"
#include "reload_transaction.h"
#include "runtime_binding.h"
#include "scene_bindings.h"
#include "script_reload.h"
#include "timer_bindings.h"
#include "touch_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/string_util.h"
#include "engine/core/thread_affinity.h"
#include "engine/core/vfs.h"
#include "engine/math/quat.h"
#include "engine/scripting/runtime_services.h"

namespace engine::scripting {

void register_generated_bindings(lua_State *L) noexcept;
namespace {

core::SimulationClock g_clock{};

/// Publishes the zero clock. Run-scoped: a run's end must zero it so a
/// later run's begin-play/start callbacks — which fire before the
/// pipeline's first per-frame publication — cannot observe the previous
/// run's time. Ordinary scene transitions keep the VM and the run alive
/// and never come through here, so the clock stays continuous across
/// engine.load_scene.
void reset_clock_bindings() noexcept {
  g_clock = core::SimulationClock{};
  refill_debug_instruction_budget();
}

/// One hot-reload watch entry: a script path and its last known mtime.
struct WatchedScript final {
  char path[512] = {};
  std::int64_t mtime = 0;
};

constexpr std::size_t kMaxWatchedScripts = 16U;
WatchedScript g_watchedScripts[kMaxWatchedScripts] = {};
std::size_t g_watchedScriptCount = 0U;

/// Returns the Lua state owned by the scripting context.
lua_State *lua_state() noexcept { return current_lua_state(); }

// Memory limit for the Lua allocator (bytes). Default 64MB.
constexpr std::size_t kDefaultMemoryLimit = 64U * 1024U * 1024U;
std::size_t g_memoryLimit = kDefaultMemoryLimit;
std::size_t g_memoryUsed = 0U;

void refresh_lua_hook() noexcept;

void refresh_lua_hook() noexcept { refresh_debug_lua_hook(); }

/// Last-resort panic logger: an unprotected Lua error is about to abort
/// the process, so record the error message before Lua calls abort().
int scripting_lua_panic(lua_State *state) noexcept {
  const char *message = lua_tostring(state, -1);
  char logBuffer[512] = {};
  std::snprintf(logBuffer, sizeof(logBuffer),
                "unprotected lua error, aborting: %s",
                (message != nullptr) ? message : "unknown lua error");
  core::log_message(core::LogLevel::Fatal, "scripting", logBuffer);
  return 0;
}

/// Carries one global-function invocation into the protected trampoline.
struct GlobalCallArgs final {
  const char *name = nullptr;
  bool hasArg = false;
  float arg = 0.0F;
  bool called = false;
};

/// Protected trampoline: looks up the named global, pushes the optional
/// float argument, and calls it; records whether a function was found so
/// metamethods and allocation failures stay catchable.
int global_call_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<GlobalCallArgs *>(lua_touserdata(state, 1));
  lua_getglobal(state, args->name);
  if (lua_isfunction(state, -1) == 0) {
    return 0;
  }
  args->called = true;
  int nargs = 0;
  if (args->hasArg) {
    lua_pushnumber(state, static_cast<lua_Number>(args->arg));
    nargs = 1;
  }
  lua_call(state, nargs, 0);
  return 0;
}

int lua_engine_start_coroutine(lua_State *state) noexcept {
  return start_lua_coroutine(
      state, static_cast<float>(g_clock.simulationSeconds), g_clock.frameIndex,
      log_lua_error, arm_debug_lua_hook);
}

// --- Entity lifecycle completeness ---

/// Registers the full Lua API on one global engine table: the manual
/// wrappers first, then the generated bindings last. Each generated
/// binding asserts that no manual wrapper already claimed its name, so a
/// name has one owner; registering them last is what lets that check see
/// every manual registration.
void register_engine_bindings(lua_State *state) noexcept {
  lua_newtable(state);

  register_entity_lifecycle_bindings(state);
  register_body_bindings(state);
  register_mesh_material_bindings(state);
  register_physics_bindings(state);

  register_input_bindings(state);

  lua_pushcfunction(state, &lua_engine_on_touch);
  lua_setfield(state, -2, "on_touch");
  lua_pushcfunction(state, &lua_engine_on_gesture);
  lua_setfield(state, -2, "on_gesture");
  lua_pushcfunction(state, &lua_engine_set_touch_mouse_emulation);
  lua_setfield(state, -2, "set_touch_mouse_emulation");

  lua_pushcfunction(state, &lua_engine_set_player_controller);
  lua_setfield(state, -2, "set_player_controller");
  lua_pushcfunction(state, &lua_engine_get_player_controller);
  lua_setfield(state, -2, "get_player_controller");

  lua_pushcfunction(state, &lua_engine_game_mode_start);
  lua_setfield(state, -2, "game_mode_start");
  lua_pushcfunction(state, &lua_engine_game_mode_pause);
  lua_setfield(state, -2, "game_mode_pause");
  lua_pushcfunction(state, &lua_engine_game_mode_end);
  lua_setfield(state, -2, "game_mode_end");
  lua_pushcfunction(state, &lua_engine_game_mode_state);
  lua_setfield(state, -2, "game_mode_state");
  lua_pushcfunction(state, &lua_engine_game_mode_set_rule);
  lua_setfield(state, -2, "game_mode_set_rule");
  lua_pushcfunction(state, &lua_engine_game_mode_get_rule);
  lua_setfield(state, -2, "game_mode_get_rule");
  lua_pushcfunction(state, &lua_engine_game_mode_max_players);
  lua_setfield(state, -2, "game_mode_max_players");

  lua_pushcfunction(state, &lua_engine_game_state_set_number);
  lua_setfield(state, -2, "game_state_set_number");
  lua_pushcfunction(state, &lua_engine_game_state_get_number);
  lua_setfield(state, -2, "game_state_get_number");
  lua_pushcfunction(state, &lua_engine_game_state_set_string);
  lua_setfield(state, -2, "game_state_set_string");
  lua_pushcfunction(state, &lua_engine_game_state_get_string);
  lua_setfield(state, -2, "game_state_get_string");
  lua_pushcfunction(state, &lua_engine_game_state_has);
  lua_setfield(state, -2, "game_state_has");
  lua_pushcfunction(state, &lua_engine_game_state_clear);
  lua_setfield(state, -2, "game_state_clear");

  lua_pushcfunction(state, &lua_engine_profiler_enable);
  lua_setfield(state, -2, "profiler_enable");
  lua_pushcfunction(state, &lua_engine_profiler_reset);
  lua_setfield(state, -2, "profiler_reset");
  lua_pushcfunction(state, &lua_engine_profiler_get_count);
  lua_setfield(state, -2, "profiler_get_count");

  lua_pushcfunction(state, &lua_engine_debugger_enable);
  lua_setfield(state, -2, "debugger_enable");
  lua_pushcfunction(state, &lua_engine_debugger_add_breakpoint);
  lua_setfield(state, -2, "debugger_add_breakpoint");
  lua_pushcfunction(state, &lua_engine_debugger_clear_breakpoints);
  lua_setfield(state, -2, "debugger_clear_breakpoints");
  lua_pushcfunction(state, &lua_engine_debugger_add_watch);
  lua_setfield(state, -2, "debugger_add_watch");
  lua_pushcfunction(state, &lua_engine_debugger_clear_watches);
  lua_setfield(state, -2, "debugger_clear_watches");
  lua_pushcfunction(state, &lua_engine_debugger_last_breakpoint);
  lua_setfield(state, -2, "debugger_last_breakpoint");
  lua_pushcfunction(state, &lua_engine_debugger_last_callstack);
  lua_setfield(state, -2, "debugger_last_callstack");
  lua_pushcfunction(state, &lua_engine_debugger_last_watch_values);
  lua_setfield(state, -2, "debugger_last_watch_values");

  register_camera_bindings(state);

  register_audio_bindings(state);

  lua_pushcfunction(state, &lua_engine_on_collision_register);
  lua_setfield(state, -2, "on_collision_handler");
  lua_pushcfunction(state, &lua_engine_remove_collision_handler);
  lua_setfield(state, -2, "remove_collision_handler");

  lua_pushcfunction(state, &lua_engine_set_anim_param);
  lua_setfield(state, -2, "set_anim_param");
  lua_pushcfunction(state, &lua_engine_on_anim_event_register);
  lua_setfield(state, -2, "on_anim_event_handler");
  lua_pushcfunction(state, &lua_engine_remove_anim_event_handler);
  lua_setfield(state, -2, "remove_anim_event_handler");

  lua_pushcfunction(state, &lua_engine_save_data);
  lua_setfield(state, -2, "save_data");
  lua_pushcfunction(state, &lua_engine_load_data);
  lua_setfield(state, -2, "load_data");

  lua_pushcfunction(state, &lua_engine_set_timeout);
  lua_setfield(state, -2, "set_timeout");
  lua_pushcfunction(state, &lua_engine_set_interval);
  lua_setfield(state, -2, "set_interval");
  lua_pushcfunction(state, &lua_engine_cancel_timer);
  lua_setfield(state, -2, "cancel_timer");

  lua_pushcfunction(state, &lua_engine_start_coroutine);
  lua_setfield(state, -2, "start_coroutine");
  lua_pushcfunction(state, &lua_engine_wait);
  lua_setfield(state, -2, "wait");
  lua_pushcfunction(state, &lua_engine_wait_frames);
  lua_setfield(state, -2, "wait_frames");
  lua_pushcfunction(state, &lua_engine_wait_until);
  lua_setfield(state, -2, "wait_until");

  register_light_bindings(state);
  register_random_bindings(state);

  register_scene_bindings(state);

  register_asset_bindings(state);

  register_entity_pool_bindings(state);

  lua_pushcfunction(state, &lua_engine_require);
  lua_setfield(state, -2, "require");

  lua_pushcfunction(state, &lua_engine_persist);
  lua_setfield(state, -2, "persist");
  lua_pushcfunction(state, &lua_engine_restore);
  lua_setfield(state, -2, "restore");

  register_generated_bindings(state);

  lua_setglobal(state, "engine");
}

/// Sandbox `load`: forwards to the base library's load (upvalue 1) with the
/// mode forced to "t", so precompiled bytecode is refused with Lua's own
/// "attempt to load a binary chunk" error whatever mode the caller named.
/// Lua's undumper trusts its input, so bytecode is never a safe chunk
/// source for author content. The chunk and chunkname pass through; an
/// explicit env (argument 4) is forwarded only when the caller supplied
/// one, because forwarding nil in its place would set the chunk's _ENV to
/// nil instead of leaving the global environment.
int text_only_load(lua_State *state) noexcept {
  const int argCount = lua_gettop(state);
  lua_settop(state, 4);
  lua_pushvalue(state, lua_upvalueindex(1));
  lua_pushvalue(state, 1);
  lua_pushvalue(state, 2);
  lua_pushliteral(state, "t");
  int forwarded = 3;
  if (argCount >= 4) {
    lua_pushvalue(state, 4);
    forwarded = 4;
  }
  lua_call(state, forwarded, LUA_MULTRET);
  return lua_gettop(state) - 4;
}

/// Protected trampoline: opens the safe library set and registers bindings.
int open_libraries_trampoline(lua_State *state) noexcept {
  luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
  lua_pop(state, 1);
  // The base library's file loaders open OS paths straight through libc,
  // outside every VFS jail check, so the sandbox keeps only the string
  // loader, and that one text-only.
  lua_pushnil(state);
  lua_setglobal(state, "dofile");
  lua_pushnil(state);
  lua_setglobal(state, "loadfile");
  lua_getglobal(state, "load");
  lua_pushcclosure(state, &text_only_load, 1);
  lua_setglobal(state, "load");
  luaL_requiref(state, LUA_COLIBNAME, luaopen_coroutine, 1);
  install_hooked_coroutine_library(state);
  lua_pop(state, 1);
  luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(state, 1);
  luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(state, 1);
  luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(state, 1);
  // The transcendentals go through the deterministic scalar set, never
  // the C library, so script-driven state matches across platforms.
  install_deterministic_math(state);
  // math.random and math.randomseed become the engine stream, so a
  // script reaching for either by habit still gets a reproducible
  // draw instead of an operating-system seeded one.
  install_engine_random_over_math(state);
  luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(state, 1);
  register_engine_bindings(state);
  return 0;
}

} // namespace

/// Accounting lua_Alloc: sandbox-gated cap, wrap-safe, counted from creation.
void *scripting_lua_alloc(void * /*ud*/, void *ptr, std::size_t osize,
                          std::size_t nsize) noexcept {
  if (ptr == nullptr) {
    osize = 0U;
  }
  if (nsize == 0U) {
    if (osize > 0U) {
      g_memoryUsed = (g_memoryUsed >= osize) ? (g_memoryUsed - osize) : 0U;
      core::mem_tracker_free(core::MemTag::Scripting, osize);
    }
    std::free(ptr);
    return nullptr;
  }
  if (nsize > osize) {
    const std::size_t growth = nsize - osize;
    const std::size_t headroom =
        std::numeric_limits<std::size_t>::max() - g_memoryUsed;
    if (debug_sandbox_enabled() && (g_memoryLimit != 0U) &&
        ((growth > headroom) || ((g_memoryUsed + growth) > g_memoryLimit))) {
      return nullptr;
    }
    void *newPtr = std::realloc(ptr, nsize);
    if (newPtr != nullptr) {
      g_memoryUsed = (growth > headroom)
                         ? std::numeric_limits<std::size_t>::max()
                         : (g_memoryUsed + growth);
      core::mem_tracker_alloc(core::MemTag::Scripting, growth);
    }
    return newPtr;
  }
  void *newPtr = std::realloc(ptr, nsize);
  if (newPtr != nullptr) {
    const std::size_t freed = osize - nsize;
    g_memoryUsed = (g_memoryUsed >= freed) ? (g_memoryUsed - freed) : 0U;
    core::mem_tracker_free(core::MemTag::Scripting, freed);
  }
  return newPtr;
}

float bindable_delta_time() noexcept {
  return static_cast<float>(g_clock.deltaSeconds);
}

float bindable_elapsed_time() noexcept {
  return static_cast<float>(g_clock.simulationSeconds);
}

int bindable_frame_count() noexcept {
  return static_cast<int>(g_clock.frameIndex);
}

int bindable_get_entity_count() noexcept {
  if (!runtime_bound()) {
    return 0;
  }
  return static_cast<int>(
      runtime_binding().services->alive_entity_count(runtime_binding().world));
}

float bindable_get_action_value(const char *name) noexcept {
  return (name != nullptr) ? core::action_value(name) : 0.0F;
}

float bindable_get_axis_value(const char *name) noexcept {
  return (name != nullptr) ? core::axis_value(name) : 0.0F;
}

bool bindable_is_alive(std::uint64_t entity) noexcept {
  if (!runtime_bound()) {
    return false;
  }
  runtime::Entity decoded{};
  return decode_entity_handle_value(entity, &decoded) &&
         runtime_binding().services->is_alive(runtime_binding().world, decoded);
}

bool bindable_has_light(std::uint64_t entity) noexcept {
  if (!runtime_bound()) {
    return false;
  }
  runtime::Entity decoded{};
  if (!decode_entity_handle_value(entity, &decoded) ||
      !runtime_binding().services->is_alive(runtime_binding().world, decoded)) {
    return false;
  }
  return runtime_binding().services->has_light_component(
      runtime_binding().world, decoded);
}

void bindable_set_camera_fov(float fov) noexcept {
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->set_camera_fov != nullptr)) {
    runtime_binding().services->set_camera_fov(fov);
  }
}

void bindable_set_master_volume(float volume) noexcept {
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->set_master_volume != nullptr)) {
    runtime_binding().services->set_master_volume(volume);
  }
}

void bindable_stop_all_sounds() noexcept {
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->stop_all_sounds != nullptr)) {
    runtime_binding().services->stop_all_sounds();
  }
}

/// Initializes the scripting system. Only safe Lua libraries are opened
/// (base, coroutine, table, string, math, utf8) — io, os, debug, and
/// package are excluded so untrusted game scripts cannot touch the file
/// system or execute system commands — and the accounting allocator is
/// active from state creation, so registration runs under pcall to keep
/// cap-induced allocation failure recoverable.
bool initialize_scripting() noexcept {
  if (lua_state() != nullptr) {
    return true;
  }

  lua_State *state = initialize_lua_state();
  if (state == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "failed to create Lua state");
    return false;
  }
  lua_atpanic(state, &scripting_lua_panic);
  set_debug_lua_state(state);
  configure_entity_script_bindings(
      state,
      EntityScriptBindingCallbacks{&push_entity_handle, &log_lua_error,
                                   &refresh_lua_hook, &core::file_mtime_ns});

  lua_pushcfunction(state, &open_libraries_trampoline);
  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    lua_pop(state, 1);
    core::log_message(core::LogLevel::Error, "scripting",
                      "failed to open Lua libraries (memory limit too low?)");
    shutdown_scripting();
    return false;
  }
  register_cheat_commands();

  refresh_lua_hook();
  return true;
}

/// Shuts down the owning system for scripting: the run-scoped state goes
/// first, while the VM is still alive to release its references, then the
/// VM and the aliases that belong to whoever destroyed it.
void shutdown_scripting() noexcept {
  reset_run_state();
  if (lua_state() != nullptr) {
    shutdown_lua_state();
  }

  g_memoryUsed = 0U;
  clear_runtime_binding();
  reset_debug_bindings();
  set_debug_lua_state(nullptr);
  // Cleared here rather than in reset_entity_script_bindings: that reset
  // also runs between runs, while the VM lives on and the alias must
  // survive. The alias belongs to whoever destroyed the VM, so it is
  // cleared beside the sibling debug alias, after shutdown_lua_state.
  clear_entity_script_bindings();
}

/// Resets run-scoped scripting state without touching the VM, the debug/DAP
/// hooks, or the runtime binding (their owners tear those down separately).
void reset_run_state() noexcept {
  lua_State *state = lua_state();
  clear_touch_gesture_callbacks(state);
  if (state != nullptr) {
    clear_persist_bindings(state);
    reset_entity_script_bindings();
    clear_lua_timer_bindings(state);
    clear_collision_handlers(state);
    clear_anim_event_handlers(state);
    clear_lua_coroutines(state);
  }
  reset_mesh_material_bindings();
  clear_deferred_mutations();
  reset_scene_bindings();
  reset_cheat_bindings();
  reset_entity_pool_bindings();
  reset_game_bindings();
  reset_clock_bindings();
  for (WatchedScript &watchedScript : g_watchedScripts) {
    watchedScript = {};
  }
  g_watchedScriptCount = 0U;
}

/// Loads the requested resource for script.
bool load_script(const char *path) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  lua_State *state = lua_state();
  if (state == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "scripting not initialized");
    return false;
  }

  if (path == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "script path is null");
    return false;
  }

  if (!protected_load_chunk(state, path, "load_script")) {
    return false;
  }

  arm_debug_lua_hook(state);

  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    log_lua_error("load_script");
    return false;
  }

  if (debug_instruction_budget_exhausted()) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "load_script: CPU instruction budget exhausted");
    return false;
  }

  return true;
}

bool call_script_function(const char *name) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  lua_State *state = lua_state();
  if (state == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "scripting not initialized");
    return false;
  }

  if (name == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "script function name is null");
    return false;
  }

  GlobalCallArgs args{};
  args.name = name;
  if (!protected_engine_dispatch(state, &global_call_trampoline, &args, 0,
                                 "call_script_function")) {
    return false;
  }

  return args.called;
}

bool call_script_function_float(const char *name, float arg) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  lua_State *state = lua_state();
  if (state == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "scripting not initialized");
    return false;
  }

  if (name == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "script function name is null");
    return false;
  }

  GlobalCallArgs args{};
  args.name = name;
  args.hasArg = true;
  args.arg = arg;
  if (!protected_engine_dispatch(state, &global_call_trampoline, &args, 0,
                                 "call_script_function_float")) {
    return false;
  }

  return args.called;
}

void dispatch_physics_callbacks(const core::Entity *pairData,
                                std::size_t pairCount) noexcept {
  dispatch_collision_handlers(lua_state(), pairData, pairCount,
                              push_entity_handle);
}

void dispatch_animation_event_callbacks() noexcept {
  dispatch_anim_event_handlers();
}

void set_simulation_clock(const core::SimulationClock &clock) noexcept {
  // A new frame index is the frame boundary the shared per-frame Lua
  // instruction budget is measured against.
  const bool frameBoundary = (clock.frameIndex != g_clock.frameIndex);
  g_clock = clock;
  if (frameBoundary) {
    refill_debug_instruction_budget();
  }
}

const core::SimulationClock &simulation_clock() noexcept { return g_clock; }

void dispatch_timers() noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  dispatch_lua_timers(lua_state());
}

void tick_timers() noexcept {
  // Advance and dispatch in one call, for a caller outside the fixed
  // step: the pipeline advances per step and dispatches per frame
  // instead, so this is what a test or tool stepping a world by hand
  // uses. The delta is the published clock's, which is the time the
  // world actually advanced.
  advance_lua_timers(lua_state(), static_cast<float>(g_clock.deltaSeconds));
  dispatch_lua_timers(lua_state());
}

// Scene transitions reset the World's TimerManager (reset_world/load_scene)
// but that layer cannot reach the scripting-side Lua registry refs, which
// would otherwise stay pinned, retaining closures (and any old-world entity
// handles their upvalues captured) past the outgoing scene's lifetime. The
// engine pipeline calls this at the same transition point as
// clear_coroutines() so no stale timer ref survives into the replacement
// world.
void clear_timers() noexcept { clear_lua_timer_bindings(lua_state()); }

void tick_coroutines() noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  // Ticks, not rendered frames: engine.wait_frames(n) waits n fixed
  // simulation steps, so a coroutine resumes at the same point in the
  // simulation whatever the frame rate (docs/decisions/0019).
  tick_lua_coroutines(lua_state(),
                      static_cast<float>(g_clock.simulationSeconds),
                      g_clock.tickIndex, log_lua_error, arm_debug_lua_hook);
}

void clear_coroutines() noexcept { clear_lua_coroutines(lua_state()); }

// Entity pools created from Lua are retired at every scene transition,
// not only at VM shutdown: otherwise each transition that used the pool
// API would leak one of the fixed 16 slots and leave any pool id a script
// still held pointing at a slot whose world content had moved on. The
// engine pipeline calls this at the same transition point as
// clear_coroutines(); the returned pool ids carry the creating world's
// content epoch (entity_pool_bindings.cpp), so a stale id from before the
// reset is rejected instead of aliasing a same-numbered replacement pool.
void clear_entity_pools() noexcept { reset_entity_pool_bindings(); }

std::size_t active_timer_ref_count() noexcept {
  return active_lua_timer_ref_count();
}

std::size_t active_entity_pool_count() noexcept { return pool_slot_count(); }

/// Modification time of a watched script read where its chunk is loaded
/// from: through the mount when the path is mounted, so a script
/// under an asset root away from the cwd hot-reloads like one beside it.
std::int64_t script_file_mtime_ns(const char *path) noexcept {
  char osPath[1024] = {};
  if (!resolve_script_os_path(path, osPath, sizeof(osPath))) {
    return 0;
  }
  return core::file_mtime_ns(osPath);
}

/// Adds a script to the hot-reload watch table (or refreshes its mtime when
/// already watched). Watching a new file no longer drops earlier watches;
/// the table is capped and overflow is logged.
void watch_script_file(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return;
  }

  for (std::size_t i = 0U; i < g_watchedScriptCount; ++i) {
    if (std::strcmp(g_watchedScripts[i].path, path) == 0) {
      g_watchedScripts[i].mtime = script_file_mtime_ns(path);
      return;
    }
  }

  if (g_watchedScriptCount >= kMaxWatchedScripts) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "script watch table full; hot reload not tracking file");
    return;
  }

  WatchedScript &entry = g_watchedScripts[g_watchedScriptCount];
  if (!copy_path_strict(entry.path, sizeof(entry.path), path,
                        "watch_script_file")) {
    return;
  }
  entry.mtime = script_file_mtime_ns(path);
  ++g_watchedScriptCount;
}

// #115c: portable rejection proof for watch_script_file's copy_path_strict
// call — unlike require/load_scene/add_script_component,
// watching a path never reads the file at registration time, so proving
// rejection needs no on-disk fixture at the truncated length and sidesteps
// the Windows MAX_PATH staging problem that left this call site's rejection
// unpinned by a red regression. A synthetic over-long string leaves this
// count unchanged; a normal path grows it by exactly one.
std::size_t watched_script_count() noexcept { return g_watchedScriptCount; }

/// Polls every watched script and reloads the ones whose mtime changed.
void check_script_reload() noexcept {
  for (std::size_t i = 0U; i < g_watchedScriptCount; ++i) {
    WatchedScript &entry = g_watchedScripts[i];
    const std::int64_t mtime = script_file_mtime_ns(entry.path);
    if ((mtime == 0) || (mtime == entry.mtime)) {
      continue;
    }

    const std::int64_t previousMtime = entry.mtime;
    entry.mtime = mtime;
    core::log_message(core::LogLevel::Info, "scripting",
                      "hot-reloading script");
    lua_State *state = lua_state();
    const ReloadOutcome outcome =
        ((state != nullptr) &&
         protected_load_chunk(state, entry.path, "hot_reload"))
            ? run_chunk_as_reload(state, "hot_reload", 0, nullptr, nullptr)
            : ReloadOutcome::RolledBack;
    if ((outcome == ReloadOutcome::Committed) ||
        (outcome == ReloadOutcome::CommitFailed)) {
      note_script_reloaded(entry.path);
    }
    switch (outcome) {
    case ReloadOutcome::Committed:
      break;
    case ReloadOutcome::Busy:
      // Nothing ran: the next poll tries this save again.
      entry.mtime = previousMtime;
      break;
    case ReloadOutcome::RolledBack:
      core::log_message(core::LogLevel::Warning, "scripting",
                        "hot-reload failed; keeping previous version");
      break;
    case ReloadOutcome::CommitFailed:
      core::log_message(core::LogLevel::Error, "scripting",
                        "hot-reload committed; some of its effects did not "
                        "apply");
      break;
    }
  }
}

// --- Sandbox configuration ---

/// Enables/disables the sandbox; the creation-time allocator enforces the
/// memory cap immediately whenever the sandbox is switched on.
void set_sandbox_enabled(bool enabled) noexcept {
  set_debug_sandbox_enabled(enabled);
  refresh_lua_hook();
}

/// Returns whether is sandbox enabled.
bool is_sandbox_enabled() noexcept { return debug_sandbox_enabled(); }

/// Sets the requested value for instruction limit.
void set_instruction_limit(int limit) noexcept {
  set_debug_instruction_limit(limit);
  refresh_lua_hook();
}

int get_instruction_limit() noexcept { return debug_instruction_limit(); }

/// Sets the requested value for memory limit.
void set_memory_limit(std::size_t limit) noexcept { g_memoryLimit = limit; }

std::size_t get_memory_limit() noexcept { return g_memoryLimit; }

std::size_t get_memory_used() noexcept { return g_memoryUsed; }

} // namespace engine::scripting
