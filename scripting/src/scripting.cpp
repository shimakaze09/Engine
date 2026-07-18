// Implements scripting behavior for the Engine Lua scripting system.

#include "engine/scripting/scripting.h"
#include "engine/scripting/bindable_api.h"
#include "engine/scripting/dap_server.h"
#include "asset_bindings.h"
#include "audio_bindings.h"
#include "binding_util.h"
#include "body_bindings.h"
#include "camera_bindings.h"
#include "collision_bindings.h"
#include "entity_lifecycle_bindings.h"
#include "light_bindings.h"
#include "mesh_material_bindings.h"
#include "physics_bindings.h"
#include "cheat_bindings.h"
#include "coroutine_bindings.h"
#include "debug_bindings.h"
#include "deferred_mutations.h"
#include "entity_handle.h"
#include "entity_pool_bindings.h"
#include "entity_script_bindings.h"
#include "game_bindings.h"
#include "input_bindings.h"
#include "lua_state.h"
#include "persist_bindings.h"
#include "runtime_binding.h"
#include "scene_bindings.h"
#include "timer_bindings.h"
#include "touch_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/string_util.h"
#include "engine/core/logging.h"
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

// Custom allocator wrapper that enforces memory limit.
void *sandbox_alloc(void * /*ud*/, void *ptr, std::size_t osize,
                    std::size_t nsize) noexcept {
  if (nsize == 0U) {
    if (osize > 0U) {
      g_memoryUsed = (g_memoryUsed >= osize) ? (g_memoryUsed - osize) : 0U;
    }
    std::free(ptr);
    return nullptr;
  }
  if (nsize > osize && (g_memoryUsed + (nsize - osize)) > g_memoryLimit) {
    return nullptr; // Memory limit exceeded.
  }
  void *newPtr = std::realloc(ptr, nsize);
  if (newPtr != nullptr) {
    if (nsize > osize) {
      g_memoryUsed += (nsize - osize);
    } else {
      const std::size_t freed = osize - nsize;
      g_memoryUsed = (g_memoryUsed >= freed) ? (g_memoryUsed - freed) : 0U;
    }
  }
  return newPtr;
}

void refresh_lua_hook() noexcept;

void refresh_lua_hook() noexcept {
  refresh_debug_lua_hook();
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
                             log_lua_error);
}

// --- Entity lifecycle completeness ---

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

  // Touch/gesture bindings (P1-M2-C3e).
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

  // Game mode state transitions and rules.
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

  // Persistent game state (survives scene transitions).
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

  // Collision handlers
  lua_pushcfunction(state, &lua_engine_on_collision_register);
  lua_setfield(state, -2, "on_collision_handler");
  lua_pushcfunction(state, &lua_engine_remove_collision_handler);
  lua_setfield(state, -2, "remove_collision_handler");

  // Timers
  lua_pushcfunction(state, &lua_engine_set_timeout);
  lua_setfield(state, -2, "set_timeout");
  lua_pushcfunction(state, &lua_engine_set_interval);
  lua_setfield(state, -2, "set_interval");
  lua_pushcfunction(state, &lua_engine_cancel_timer);
  lua_setfield(state, -2, "cancel_timer");

  // Coroutines
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

  // Utility module loader — load a Lua file as a shared module (cached).
  lua_pushcfunction(state, &lua_engine_require);
  lua_setfield(state, -2, "require");

  // Generated bindings override a curated subset of manual wrappers.
  register_generated_bindings(state);

  // Hot-reload state preservation.
  lua_pushcfunction(state, &lua_engine_persist);
  lua_setfield(state, -2, "persist");
  lua_pushcfunction(state, &lua_engine_restore);
  lua_setfield(state, -2, "restore");

  lua_setglobal(state, "engine");
}

} // namespace


float bindable_delta_time() noexcept { return g_deltaSeconds; }

float bindable_elapsed_time() noexcept { return g_totalSeconds; }

int bindable_frame_count() noexcept { return static_cast<int>(g_frameIndex); }

int bindable_get_entity_count() noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr)) {
    return 0;
  }
  return static_cast<int>(runtime_binding().services->get_transform_count(runtime_binding().world));
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
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_camera_fov != nullptr)) {
    runtime_binding().services->set_camera_fov(fov);
  }
}

void bindable_set_master_volume(float volume) noexcept {
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_master_volume != nullptr)) {
    runtime_binding().services->set_master_volume(volume);
  }
}

void bindable_stop_all_sounds() noexcept {
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->stop_all_sounds != nullptr)) {
    runtime_binding().services->stop_all_sounds();
  }
}

/// Initializes the owning system for scripting.
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
  set_debug_lua_state(state);
  configure_entity_script_bindings(
      state, EntityScriptBindingCallbacks{&push_entity_handle, &log_lua_error,
                                          &refresh_lua_hook, &get_file_mtime});

  // Open only safe libraries. io, os, debug, and package are excluded to
  // prevent untrusted game scripts from accessing the file system or executing
  // arbitrary system commands.
  luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
  lua_pop(state, 1);
  luaL_requiref(state, LUA_COLIBNAME, luaopen_coroutine, 1);
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
  register_cheat_commands();

  // Install sandboxed memory allocator. io/os/debug libraries are already
  // excluded (only base, coroutine, table, string, math, utf8 are opened).
  if (debug_sandbox_enabled()) {
    lua_setallocf(state, sandbox_alloc, nullptr);
  }

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
    clear_lua_coroutines(state);
    shutdown_lua_state();
  }

  clear_runtime_binding();
  reset_mesh_material_bindings();
  clear_deferred_mutations();
  reset_scene_bindings();
  reset_debug_bindings();
  set_debug_lua_state(nullptr);
  reset_cheat_bindings();
  reset_entity_pool_bindings();
  reset_game_bindings();
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

  if (luaL_loadfile(state, path) != LUA_OK) {
    log_lua_error("load_script");
    return false;
  }

  refresh_lua_hook(); // Reset instruction counter for this invocation.

  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    log_lua_error("load_script");
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

  lua_getglobal(state, name);
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    return false;
  }

  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    log_lua_error("call_script_function");
    return false;
  }

  return true;
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

  lua_getglobal(state, name);
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    return false;
  }

  lua_pushnumber(state, static_cast<lua_Number>(arg));
  if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
    log_lua_error("call_script_function_float");
    return false;
  }

  return true;
}

void dispatch_physics_callbacks(const std::uint32_t *pairData,
                                std::size_t pairCount) noexcept {
  dispatch_collision_handlers(lua_state(), pairData, pairCount,
                              push_entity_handle_from_index, log_lua_error);
}

namespace {
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
  // Use nanosecond-precision mtime when available so sub-second file writes
  // are detected (st_mtime alone has 1-second granularity on many POSIX FS).
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

/// Sets the requested value for frame index.
void set_frame_index(std::uint32_t frameIndex) noexcept {
  g_frameIndex = frameIndex;
}

void tick_timers() noexcept {
  tick_lua_timers(lua_state(), g_deltaSeconds);
}

void tick_coroutines() noexcept {
  tick_lua_coroutines(lua_state(), g_totalSeconds, g_frameIndex, log_lua_error,
                      refresh_lua_hook);
}

void clear_coroutines() noexcept {
  clear_lua_coroutines(lua_state());
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
  core::copy_string(entry.path, sizeof(entry.path), path);
  entry.mtime = get_file_mtime(path);
  ++g_watchedScriptCount;
}

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
    if (!load_script(entry.path)) {
      core::log_message(core::LogLevel::Warning, "scripting",
                        "hot-reload failed; keeping previous version");
    }
  }
}

// --- Sandbox configuration ---

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

} // namespace engine::scripting
