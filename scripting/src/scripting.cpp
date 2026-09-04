// Implements scripting behavior for the Engine Lua scripting system.

#include "engine/scripting/scripting.h"
#include "asset_bindings.h"
#include "audio_bindings.h"
#include "binding_util.h"
#include "body_bindings.h"
#include "camera_bindings.h"
#include "cheat_bindings.h"
#include "animation_bindings.h"
#include "collision_bindings.h"
#include "coroutine_bindings.h"
#include "debug_bindings.h"
#include "deferred_mutations.h"
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
#include "runtime_binding.h"
#include "scene_bindings.h"
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
#include "engine/core/string_util.h"
#include "engine/math/quat.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace engine::scripting {

void register_generated_bindings(lua_State *L) noexcept;
namespace {

float g_deltaSeconds = 0.0F;
float g_totalSeconds = 0.0F;
std::uint32_t g_frameIndex = 0U;

/// Returns the Lua-visible clocks (delta/elapsed/frame index) to their
/// initial values. Run-scoped: a run's end must zero them so a later run's
/// begin-play/start callbacks — which fire before the pipeline's first
/// per-frame publication — cannot observe the previous run's time. Ordinary
/// scene transitions keep the VM and the run alive and never come through
/// here, so clocks stay continuous across engine.load_scene.
void reset_clock_bindings() noexcept {
  g_deltaSeconds = 0.0F;
  g_totalSeconds = 0.0F;
  g_frameIndex = 0U;
}

/// One hot-reload watch entry: a script path and its last known mtime.
struct WatchedScript final {
  char path[512] = {};
  std::int64_t mtime = 0;
};

constexpr std::size_t kMaxWatchedScripts = 16U;
WatchedScript g_watchedScripts[kMaxWatchedScripts] = {};
std::size_t g_watchedScriptCount = 0U;

std::int64_t get_file_mtime(const char *path) noexcept;

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

int lua_engine_delta_time(lua_State *state) noexcept {
  lua_pushnumber(state, static_cast<lua_Number>(g_deltaSeconds));
  return 1;
}

int lua_engine_elapsed_time(lua_State *state) noexcept {
  lua_pushnumber(state, static_cast<lua_Number>(g_totalSeconds));
  return 1;
}

int lua_engine_frame_count(lua_State *state) noexcept {
  lua_pushinteger(state, static_cast<lua_Integer>(g_frameIndex));
  return 1;
}

int lua_engine_start_coroutine(lua_State *state) noexcept {
  return start_lua_coroutine(state, g_totalSeconds, g_frameIndex,
                             log_lua_error, arm_debug_lua_hook);
}

// --- Entity lifecycle completeness ---

/// Registers the full Lua API on one global engine table; generated
/// bindings are registered last and override a curated subset of the
/// manual wrappers.
void register_engine_bindings(lua_State *state) noexcept {
  lua_newtable(state);

  register_entity_lifecycle_bindings(state);
  register_body_bindings(state);
  register_mesh_material_bindings(state);
  register_physics_bindings(state);

  lua_pushcfunction(state, &lua_engine_delta_time);
  lua_setfield(state, -2, "delta_time");

  lua_pushcfunction(state, &lua_engine_elapsed_time);
  lua_setfield(state, -2, "elapsed_time");

  register_input_bindings(state);

  lua_pushcfunction(state, &lua_engine_on_touch);
  lua_setfield(state, -2, "on_touch");
  lua_pushcfunction(state, &lua_engine_on_gesture);
  lua_setfield(state, -2, "on_gesture");
  lua_pushcfunction(state, &lua_engine_set_touch_mouse_emulation);
  lua_setfield(state, -2, "set_touch_mouse_emulation");

  lua_pushcfunction(state, &lua_engine_set_game_mode);
  lua_setfield(state, -2, "set_game_mode");
  lua_pushcfunction(state, &lua_engine_get_game_mode);
  lua_setfield(state, -2, "get_game_mode");
  lua_pushcfunction(state, &lua_engine_set_game_state);
  lua_setfield(state, -2, "set_game_state");
  lua_pushcfunction(state, &lua_engine_get_game_state);
  lua_setfield(state, -2, "get_game_state");
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

  register_cheat_status_bindings(state);

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

  lua_pushcfunction(state, &lua_engine_frame_count);
  lua_setfield(state, -2, "frame_count");

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

  register_scene_bindings(state);

  register_asset_bindings(state);

  register_entity_pool_bindings(state);

  lua_pushcfunction(state, &lua_engine_require);
  lua_setfield(state, -2, "require");

  register_generated_bindings(state);

  lua_pushcfunction(state, &lua_engine_persist);
  lua_setfield(state, -2, "persist");
  lua_pushcfunction(state, &lua_engine_restore);
  lua_setfield(state, -2, "restore");

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
    }
    return newPtr;
  }
  void *newPtr = std::realloc(ptr, nsize);
  if (newPtr != nullptr) {
    const std::size_t freed = osize - nsize;
    g_memoryUsed = (g_memoryUsed >= freed) ? (g_memoryUsed - freed) : 0U;
  }
  return newPtr;
}

float bindable_delta_time() noexcept { return g_deltaSeconds; }

float bindable_elapsed_time() noexcept { return g_totalSeconds; }

int bindable_frame_count() noexcept { return static_cast<int>(g_frameIndex); }

int bindable_get_entity_count() noexcept {
  if ((runtime_binding().world == nullptr) ||
      (runtime_binding().services == nullptr)) {
    return 0;
  }
  return static_cast<int>(
      runtime_binding().services->get_entity_count(runtime_binding().world));
}

bool bindable_is_gamepad_connected() noexcept {
  return core::is_gamepad_connected();
}

bool bindable_is_key_down(int scancode) noexcept {
  return core::is_key_down(scancode);
}

bool bindable_is_key_pressed(int scancode) noexcept {
  return core::is_key_pressed(scancode);
}

bool bindable_is_gamepad_button_down(int button) noexcept {
  return core::is_gamepad_button_down(button);
}

bool bindable_is_action_down(const char *name) noexcept {
  return (name != nullptr) ? core::is_action_down(name) : false;
}

bool bindable_is_action_pressed(const char *name) noexcept {
  return (name != nullptr) ? core::is_action_pressed(name) : false;
}

float bindable_get_action_value(const char *name) noexcept {
  return (name != nullptr) ? core::action_value(name) : 0.0F;
}

float bindable_get_axis_value(const char *name) noexcept {
  return (name != nullptr) ? core::axis_value(name) : 0.0F;
}

bool bindable_is_alive(std::uint64_t entity) noexcept {
  if (runtime_binding().world == nullptr) {
    return false;
  }
  runtime::Entity decoded{};
  return decode_entity_handle_value(entity, &decoded) &&
         runtime_binding().world->is_alive(decoded);
}

bool bindable_has_light(std::uint64_t entity) noexcept {
  if (runtime_binding().world == nullptr) {
    return false;
  }
  runtime::Entity decoded{};
  if (!decode_entity_handle_value(entity, &decoded) ||
      !runtime_binding().world->is_alive(decoded)) {
    return false;
  }
  return runtime_binding().world->has_light_component(decoded);
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
      state, EntityScriptBindingCallbacks{&push_entity_handle, &log_lua_error,
                                          &refresh_lua_hook, &get_file_mtime});

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

/// Shuts down the owning system for scripting.
void shutdown_scripting() noexcept {
  lua_State *state = lua_state();
  clear_touch_gesture_callbacks(state);

  if (state != nullptr) {
    clear_persist_bindings(state);
    reset_entity_script_bindings();
    clear_lua_timer_bindings(state);
    clear_collision_handlers(state);
    clear_anim_event_handlers(state);
    clear_lua_coroutines(state);
    shutdown_lua_state();
  }

  g_memoryUsed = 0U;
  clear_runtime_binding();
  reset_mesh_material_bindings();
  clear_deferred_mutations();
  reset_scene_bindings();
  reset_debug_bindings();
  set_debug_lua_state(nullptr);
  // Cleared here rather than in reset_entity_script_bindings: that reset
  // also runs between runs, while the VM lives on and the alias must
  // survive. The alias belongs to whoever destroyed the VM, so it is
  // cleared beside the sibling debug alias, after shutdown_lua_state.
  clear_entity_script_bindings();
  reset_cheat_bindings();
  reset_entity_pool_bindings();
  reset_game_bindings();
  reset_clock_bindings();
  for (WatchedScript &watchedScript : g_watchedScripts) {
    watchedScript = {};
  }
  g_watchedScriptCount = 0U;
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

/// Sets the requested value for frame time.
void set_frame_time(float deltaSeconds, float totalSeconds) noexcept {
  g_deltaSeconds = deltaSeconds;
  g_totalSeconds = totalSeconds;
  if (dap_is_running()) {
    dap_poll();
  }
}

/// Loads the requested resource for script.
bool load_script(const char *path) noexcept {
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

namespace {

/// Carries the globals snapshot registry reference across the protected
/// snapshot/restore trampolines.
struct GlobalsSnapshotArgs final {
  int ref = LUA_NOREF;
};

// Rollback covers tables (and, since #115a, their metatables) reachable
// from _G through table fields down to this depth, plus (#199) the
// upvalue cells of every Lua closure met during that walk; deeper tables,
// C-closure upvalues, userdata, and registry-only state stay shared and
// are not rolled back. Upvalue restore is cell-identity based: Lua joins
// upvalues across closures created by the same enclosing function into
// one shared cell (lua_upvalueid names it), so each cell is snapshotted
// and restored exactly once through any one holder — lua_setupvalue
// writes through the shared cell, never rebinding it, which preserves the
// aliasing relationships the script depends on.
constexpr std::size_t kMaxReloadSnapshotDepth = 8U;

/// Replaces the table on top of the stack with its snapshot copy,
/// recording orig->copy in memo and copy->orig in rev; cycles reuse the
/// memoized copy, and depth/stack limits fall back to a shared reference.
/// Also records the table's metatable identity in metaIndex (#115a) and
/// walks into it the same way, so a failed reload that swaps, clears, or
/// mutates a metatable (a common OOP class-table pattern) rolls back too.
/// Lua closures met along the walk are collected into closuresIndex
/// (fn -> true) for the upvalue snapshot pass (#199).
void deep_snapshot_table(lua_State *state, int memoIndex, int revIndex,
                         int metaIndex, int closuresIndex,
                         std::size_t depth) noexcept {
  const int origIndex = lua_absindex(state, -1);
  lua_pushvalue(state, origIndex);
  lua_rawget(state, memoIndex);
  if (!lua_isnil(state, -1)) {
    lua_replace(state, origIndex);
    return;
  }
  lua_pop(state, 1);

  lua_newtable(state);
  const int copyIndex = lua_absindex(state, -1);
  lua_pushvalue(state, origIndex);
  lua_pushvalue(state, copyIndex);
  lua_rawset(state, memoIndex);
  lua_pushvalue(state, copyIndex);
  lua_pushvalue(state, origIndex);
  lua_rawset(state, revIndex);

  if (((depth + 1U) < kMaxReloadSnapshotDepth) &&
      (lua_checkstack(state, 8) != 0) &&
      (lua_getmetatable(state, origIndex) != 0)) {
    // Record the ORIGINAL metatable's identity before recursing — the
    // recursive call replaces this stack slot with the metatable's own
    // snapshot copy, which restore never needs directly (its fields
    // restore through its own memo entry; only the identity link back to
    // origIndex needs to survive here).
    lua_pushvalue(state, origIndex);
    lua_pushvalue(state, -2);
    lua_rawset(state, metaIndex);
    deep_snapshot_table(state, memoIndex, revIndex, metaIndex, closuresIndex,
                        depth + 1U);
    lua_pop(state, 1);
  }

  lua_pushnil(state);
  while (lua_next(state, origIndex) != 0) {
    if ((lua_istable(state, -1) != 0) &&
        ((depth + 1U) < kMaxReloadSnapshotDepth) &&
        (lua_checkstack(state, 8) != 0)) {
      deep_snapshot_table(state, memoIndex, revIndex, metaIndex, closuresIndex,
                          depth + 1U);
    } else if ((lua_isfunction(state, -1) != 0) &&
               (lua_iscfunction(state, -1) == 0)) {
      // #199: remember every reachable Lua closure (dedup in the hash
      // part, ordered in the array part — the upvalue pass appends while
      // iterating, so no lua_next runs over a growing table); C closures
      // stay owned by their bindings.
      lua_pushvalue(state, -1);
      lua_rawget(state, closuresIndex);
      const bool closureKnown = !lua_isnil(state, -1);
      lua_pop(state, 1);
      if (!closureKnown) {
        lua_pushvalue(state, -1);
        lua_pushboolean(state, 1);
        lua_rawset(state, closuresIndex);
        lua_pushvalue(state, -1);
        lua_rawseti(state, closuresIndex,
                    static_cast<lua_Integer>(lua_rawlen(state, closuresIndex)) +
                        1);
      }
    }
    lua_pushvalue(state, -2);
    lua_pushvalue(state, -2);
    lua_rawset(state, copyIndex);
    lua_pop(state, 1);
  }

  lua_replace(state, origIndex);
}

/// Protected trampoline: deep-copies the globals table (and every nested
/// table within the depth cap) into a memoized snapshot refed into the
/// registry, so allocation failure while snapshotting stays catchable.
int snapshot_globals_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<GlobalsSnapshotArgs *>(lua_touserdata(state, 1));
  lua_createtable(state, 5, 0);
  const int containerIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int memoIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int revIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int metaIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int closuresIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int cellsIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int holdersIndex = lua_absindex(state, -1);

  lua_pushglobaltable(state);
  deep_snapshot_table(state, memoIndex, revIndex, metaIndex, closuresIndex,
                      0U);
  lua_pop(state, 1);

  // #199: snapshot every collected closure's upvalue cells. Cells are
  // keyed by lua_upvalueid identity so a cell shared across closures is
  // recorded (and later restored) exactly once; a table-valued cell is
  // additionally deep-snapshotted so its contents roll back in place.
  lua_Integer holderCount = 0;
  for (lua_Integer closure = 1;
       closure <= static_cast<lua_Integer>(lua_rawlen(state, closuresIndex));
       ++closure) {
    lua_rawgeti(state, closuresIndex, closure);
    const int fnIndex = lua_absindex(state, -1);
    for (int upvalue = 1;; ++upvalue) {
      if (lua_checkstack(state, 10) == 0) {
        break;
      }
      const char *name = lua_getupvalue(state, fnIndex, upvalue);
      if (name == nullptr) {
        break;
      }
      void *cellId = lua_upvalueid(state, fnIndex, upvalue);

      lua_pushlightuserdata(state, cellId);
      lua_rawget(state, cellsIndex);
      const bool cellKnown = !lua_isnil(state, -1);
      lua_pop(state, 1);
      if (!cellKnown) {
        if (lua_istable(state, -1) != 0) {
          lua_pushvalue(state, -1);
          deep_snapshot_table(state, memoIndex, revIndex, metaIndex,
                              closuresIndex, 0U);
          lua_pop(state, 1);
        }
        lua_pushlightuserdata(state, cellId);
        lua_pushvalue(state, -2);
        lua_rawset(state, cellsIndex);
      }

      // holders[n] = {fn, upvalueIndex, cellId}: restore needs one live
      // closure per cell to write through.
      lua_createtable(state, 3, 0);
      lua_pushvalue(state, fnIndex);
      lua_rawseti(state, -2, 1);
      lua_pushinteger(state, upvalue);
      lua_rawseti(state, -2, 2);
      lua_pushlightuserdata(state, cellId);
      lua_rawseti(state, -2, 3);
      lua_rawseti(state, holdersIndex, ++holderCount);

      lua_pop(state, 1);
    }
    lua_pop(state, 1);
  }

  lua_pushvalue(state, memoIndex);
  lua_rawseti(state, containerIndex, 1);
  lua_pushvalue(state, revIndex);
  lua_rawseti(state, containerIndex, 2);
  lua_pushvalue(state, metaIndex);
  lua_rawseti(state, containerIndex, 3);
  lua_pushvalue(state, cellsIndex);
  lua_rawseti(state, containerIndex, 4);
  lua_pushvalue(state, holdersIndex);
  lua_rawseti(state, containerIndex, 5);
  lua_pop(state, 6);
  args->ref = luaL_ref(state, LUA_REGISTRYINDEX);
  return 0;
}

/// Captures globals (nested tables included, up to the depth cap) for
/// rollback after a failed reload; false (with the error logged) when
/// snapshotting itself fails.
bool snapshot_global_bindings(lua_State *state, int *outReference) noexcept {
  GlobalsSnapshotArgs args{};
  if (!protected_c_operation(state, &snapshot_globals_trampoline, &args, 0,
                             "hot_reload globals snapshot")) {
    return false;
  }
  *outReference = args.ref;
  return true;
}

/// Restores one snapshotted table in place from its copy: keys added
/// since the snapshot are removed, snapshot keys are reassigned, and
/// values that are copies of snapshotted tables map back (via rev) to the
/// original table object so shared references keep their identity.
void restore_table_in_place(lua_State *state, int revIndex, int origIndex,
                            int copyIndex) noexcept {
  lua_pushnil(state);
  while (lua_next(state, origIndex) != 0) {
    lua_pop(state, 1);
    lua_pushvalue(state, -1);
    lua_rawget(state, copyIndex);
    const bool wasPresent = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (!wasPresent) {
      lua_pushvalue(state, -1);
      lua_pushnil(state);
      lua_rawset(state, origIndex);
    }
  }

  lua_pushnil(state);
  while (lua_next(state, copyIndex) != 0) {
    lua_pushvalue(state, -2);
    if (lua_istable(state, -2) != 0) {
      lua_pushvalue(state, -2);
      lua_rawget(state, revIndex);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_pushvalue(state, -2);
      }
    } else {
      lua_pushvalue(state, -2);
    }
    lua_rawset(state, origIndex);
    lua_pop(state, 1);
  }
}

/// Protected trampoline: restores every snapshotted table in place (the
/// globals table is one memo entry), so allocation failure while
/// rebuilding tables stays catchable.
int restore_globals_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<GlobalsSnapshotArgs *>(lua_touserdata(state, 1));
  const int originalTop = lua_gettop(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, args->ref);
  if (!lua_istable(state, -1)) {
    lua_settop(state, originalTop);
    return 0;
  }
  const int containerIndex = lua_absindex(state, -1);
  lua_rawgeti(state, containerIndex, 1);
  const int memoIndex = lua_absindex(state, -1);
  lua_rawgeti(state, containerIndex, 2);
  const int revIndex = lua_absindex(state, -1);
  lua_rawgeti(state, containerIndex, 3);
  const int metaIndex = lua_absindex(state, -1);
  if ((lua_istable(state, memoIndex) == 0) ||
      (lua_istable(state, revIndex) == 0) ||
      (lua_istable(state, metaIndex) == 0) ||
      (lua_checkstack(state, 16) == 0)) {
    lua_settop(state, originalTop);
    return 0;
  }

  lua_pushnil(state);
  while (lua_next(state, memoIndex) != 0) {
    const int origIndex = lua_absindex(state, -2);
    const int copyIndex = lua_absindex(state, -1);
    restore_table_in_place(state, revIndex, origIndex, copyIndex);
    // #115a: reattach the table's original metatable identity (nil clears
    // one the failed reload added); the metatable's own fields, if it is
    // itself a snapshotted table, were just restored by this same loop.
    lua_pushvalue(state, origIndex);
    lua_rawget(state, metaIndex);
    lua_setmetatable(state, origIndex);
    lua_pop(state, 1);
  }

  // #199: restore each snapshotted upvalue cell exactly once through any
  // one recorded holder — lua_setupvalue writes through the shared cell,
  // so every closure aliasing it sees the restored value and the sharing
  // relationship itself is untouched. Pre-#199 snapshots carry no
  // holders/cells slots and skip this pass.
  lua_rawgeti(state, containerIndex, 4);
  const int cellsIndex = lua_absindex(state, -1);
  lua_rawgeti(state, containerIndex, 5);
  const int holdersIndex = lua_absindex(state, -1);
  if ((lua_istable(state, cellsIndex) != 0) &&
      (lua_istable(state, holdersIndex) != 0)) {
    lua_newtable(state);
    const int doneIndex = lua_absindex(state, -1);
    const auto holderCount =
        static_cast<lua_Integer>(lua_rawlen(state, holdersIndex));
    for (lua_Integer holder = 1; holder <= holderCount; ++holder) {
      lua_rawgeti(state, holdersIndex, holder);
      const int tripleIndex = lua_absindex(state, -1);
      lua_rawgeti(state, tripleIndex, 3);
      lua_pushvalue(state, -1);
      lua_rawget(state, doneIndex);
      const bool cellDone = !lua_isnil(state, -1);
      lua_pop(state, 1);
      if (cellDone) {
        lua_pop(state, 2);
        continue;
      }
      lua_pushvalue(state, -1);
      lua_pushboolean(state, 1);
      lua_rawset(state, doneIndex);

      lua_rawgeti(state, tripleIndex, 1);
      const int fnIndex = lua_absindex(state, -1);
      lua_rawgeti(state, tripleIndex, 2);
      const auto upvalue = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_pushvalue(state, -2);
      lua_rawget(state, cellsIndex);
      if (lua_setupvalue(state, fnIndex, upvalue) == nullptr) {
        lua_pop(state, 1); // unreachable index: setupvalue pops nothing
      }
      lua_pop(state, 3);
    }
    lua_pop(state, 1);
  }

  lua_settop(state, originalTop);
  return 0;
}

/// Restores snapshotted tables under protection; a restore that itself
/// hits allocation failure is logged (tables may then be partially
/// restored — unavoidable under OOM).
void restore_global_bindings(lua_State *state, int snapshotReference) noexcept {
  GlobalsSnapshotArgs args{};
  args.ref = snapshotReference;
  static_cast<void>(protected_c_operation(state, &restore_globals_trampoline,
                                          &args, 0,
                                          "hot_reload globals restore"));
}

/// Executes a reload, rolling back a failed chunk's top-level Lua bindings
/// and the deferred scene request it queued. The scene request rolls back
/// with them because it outlives the chunk: the runtime commits it after the
/// frame, so a chunk that asks for a new scene and then fails would destroy
/// the live World on the strength of bindings that were just discarded.
/// A prior request the chunk overwrote is restored for the same reason —
/// the failed chunk is not entitled to redirect a transition already queued.
/// TODO(#343): timer, audio, and ECS mutations a failed chunk performed are
/// not rolled back.
bool reload_script_transactionally(const char *path) noexcept {
  lua_State *state = lua_state();
  if ((state == nullptr) || (path == nullptr)) {
    return false;
  }

  if (!protected_load_chunk(state, path, "hot_reload")) {
    return false;
  }

  int snapshotReference = LUA_NOREF;
  if (!snapshot_global_bindings(state, &snapshotReference)) {
    lua_pop(state, 1);
    return false;
  }
  // Captured after the chunk is loaded and before it runs: loading executes
  // no chunk code, so this is the request as it stood before the reload.
  const PendingSceneOpCheckpoint sceneOpCheckpoint = capture_pending_scene_op();
  arm_debug_lua_hook(state);
  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    log_lua_error("hot_reload");
    restore_global_bindings(state, snapshotReference);
    restore_pending_scene_op(sceneOpCheckpoint);
    luaL_unref(state, LUA_REGISTRYINDEX, snapshotReference);
    return false;
  }

  if (debug_instruction_budget_exhausted()) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "hot_reload: CPU instruction budget exhausted");
    restore_global_bindings(state, snapshotReference);
    restore_pending_scene_op(sceneOpCheckpoint);
    luaL_unref(state, LUA_REGISTRYINDEX, snapshotReference);
    return false;
  }

  luaL_unref(state, LUA_REGISTRYINDEX, snapshotReference);
  return true;
}

/// File mtime with nanosecond precision where the platform provides it,
/// so sub-second writes are detected (st_mtime alone has 1-second
/// granularity on many POSIX filesystems).
std::int64_t get_file_mtime(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return 0;
  }
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
    return 0;
  }
  ULARGE_INTEGER ul{};
  ul.LowPart = data.ftLastWriteTime.dwLowDateTime;
  ul.HighPart = data.ftLastWriteTime.dwHighDateTime;
  return static_cast<std::int64_t>(ul.QuadPart);
#else
  struct stat st{};
  if (stat(path, &st) != 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL +
         static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#elif defined(__linux__)
  return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
         static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#else
  return static_cast<std::int64_t>(st.st_mtime);
#endif
#endif
}
} // anonymous namespace

/// Frame boundary: advances the frame index and refills the shared
/// per-frame Lua instruction budget (issue #84, one budget per frame).
void set_frame_index(std::uint32_t frameIndex) noexcept {
  g_frameIndex = frameIndex;
  refill_debug_instruction_budget();
}

void tick_timers() noexcept { tick_lua_timers(lua_state(), g_deltaSeconds); }

// H-16 remainder (#93a): scene transitions reset the World's TimerManager
// (reset_world/load_scene) but that layer cannot reach the scripting-side
// Lua registry refs, so a transition mid-flight left them pinned, retaining
// closures (and any old-world entity handles their upvalues captured) past
// the outgoing scene's lifetime. Mirror clear_coroutines(): the engine
// pipeline calls this at the same transition point so no stale timer ref
// survives into the replacement world.
void clear_timers() noexcept { clear_lua_timer_bindings(lua_state()); }

void tick_coroutines() noexcept {
  tick_lua_coroutines(lua_state(), g_totalSeconds, g_frameIndex, log_lua_error,
                      arm_debug_lua_hook);
}

void clear_coroutines() noexcept { clear_lua_coroutines(lua_state()); }

// H-16 remainder (#93b): entity pools created from Lua were never retired
// at a scene transition, only at full VM shutdown, so every transition that
// used the pool API leaked one of the fixed 16 slots and left any pool id a
// script still held pointing at a slot whose world content had moved on.
// The engine pipeline now calls this at the same transition point as
// clear_coroutines(); the returned pool ids carry the creating world's
// content epoch (entity_pool_bindings.cpp), so a stale id from before the
// reset is rejected instead of aliasing a same-numbered replacement pool.
void clear_entity_pools() noexcept { reset_entity_pool_bindings(); }

std::size_t active_timer_ref_count() noexcept {
  return active_lua_timer_ref_count();
}

std::size_t active_entity_pool_count() noexcept { return pool_slot_count(); }

/// Adds a script to the hot-reload watch table (or refreshes its mtime when
/// already watched). Watching a new file no longer drops earlier watches;
/// the table is capped and overflow is logged.
void watch_script_file(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return;
  }

  for (std::size_t i = 0U; i < g_watchedScriptCount; ++i) {
    if (std::strcmp(g_watchedScripts[i].path, path) == 0) {
      g_watchedScripts[i].mtime = get_file_mtime(path);
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
  entry.mtime = get_file_mtime(path);
  ++g_watchedScriptCount;
}

// #115c: portable rejection proof for watch_script_file's copy_path_strict
// call (issue #80/77e6dfe) — unlike require/load_scene/add_script_component,
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
    const std::int64_t mtime = get_file_mtime(entry.path);
    if ((mtime == 0) || (mtime == entry.mtime)) {
      continue;
    }

    entry.mtime = mtime;
    core::log_message(core::LogLevel::Info, "scripting",
                      "hot-reloading script");
    if (!reload_script_transactionally(entry.path)) {
      core::log_message(core::LogLevel::Warning, "scripting",
                        "hot-reload failed; keeping previous version");
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
