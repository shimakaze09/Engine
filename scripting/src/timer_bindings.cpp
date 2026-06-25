// Owns Lua timer bindings for the Engine scripting system.

#include "timer_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>
#include <cstdio>

#include "engine/core/logging.h"
#include "engine/runtime/timer_manager.h"
#include "engine/runtime/world.h"
#include "runtime_binding.h"

namespace engine::scripting {
namespace {

constexpr std::size_t kMaxTimerRefs = runtime::TimerManager::kMaxTimers;
int g_timerLuaRefs[kMaxTimerRefs];
bool g_timerRefsInit = false;
lua_State *g_timerLuaState = nullptr;

/// Initializes Lua timer reference storage on first use.
void ensure_timer_refs_init() noexcept {
  if (g_timerRefsInit) {
    return;
  }

  for (std::size_t i = 0U; i < kMaxTimerRefs; ++i) {
    g_timerLuaRefs[i] = LUA_NOREF;
  }
  g_timerRefsInit = true;
}

/// Returns the Lua state that owns timer registry refs.
lua_State *timer_ref_state(lua_State *fallbackState) noexcept {
  return (g_timerLuaState != nullptr) ? g_timerLuaState : fallbackState;
}

/// Logs a Lua timer callback error with traceback and pops error values.
void log_timer_lua_error(lua_State *state, const char *context) noexcept {
  if (state == nullptr) {
    return;
  }

  const char *message = lua_tostring(state, -1);
  if (message == nullptr) {
    message = "unknown lua error";
  }

  luaL_traceback(state, state, message, 1);
  const char *trace = lua_tostring(state, -1);
  if (trace == nullptr) {
    trace = message;
  }

  char logBuffer[1024] = {};
  if ((context != nullptr) && (context[0] != '\0')) {
    std::snprintf(logBuffer, sizeof(logBuffer), "lua error (%s): %s", context,
                  trace);
  } else {
    std::snprintf(logBuffer, sizeof(logBuffer), "lua error: %s", trace);
  }
  core::log_message(core::LogLevel::Error, "scripting", logBuffer);
  lua_pop(state, 2);
}

/// Invokes a Lua callback for a fired runtime timer.
void lua_timer_callback(runtime::TimerId id, void *userData) noexcept {
  (void)userData;
  if ((g_timerLuaState == nullptr) || (id == runtime::kInvalidTimerId) ||
      (runtime_binding().world == nullptr)) {
    return;
  }

  auto &timerManager = runtime_binding().world->timer_manager();
  const std::size_t slot = timerManager.slot_for_id(id);
  if (slot >= kMaxTimerRefs) {
    return;
  }

  const bool wasRepeating = timerManager.entry_at(slot).repeat;
  const int ref = g_timerLuaRefs[slot];
  if (ref == LUA_NOREF) {
    return;
  }

  lua_rawgeti(g_timerLuaState, LUA_REGISTRYINDEX, ref);
  if (lua_isfunction(g_timerLuaState, -1)) {
    if (lua_pcall(g_timerLuaState, 0, 0, 0) != LUA_OK) {
      log_timer_lua_error(g_timerLuaState, "timer");
    }
  } else {
    lua_pop(g_timerLuaState, 1);
  }

  if (runtime_binding().world == nullptr) {
    return;
  }

  auto &currentTimerManager = runtime_binding().world->timer_manager();
  const bool stillCurrent =
      (currentTimerManager.slot_for_id(id) == slot) &&
      currentTimerManager.entry_at(slot).active;
  if (!wasRepeating || !stillCurrent) {
    if (g_timerLuaRefs[slot] != LUA_NOREF) {
      luaL_unref(g_timerLuaState, LUA_REGISTRYINDEX, g_timerLuaRefs[slot]);
      g_timerLuaRefs[slot] = LUA_NOREF;
    }
  }
}

/// Restores Lua callbacks on timers loaded from serialized snapshots.
void rewire_lua_timer_callbacks() noexcept {
  if (runtime_binding().world == nullptr) {
    return;
  }

  ensure_timer_refs_init();
  auto &timerManager = runtime_binding().world->timer_manager();
  for (std::size_t i = 0U; i < kMaxTimerRefs; ++i) {
    auto &entry = timerManager.entry_at_mut(i);
    if (entry.active && (entry.callback == nullptr) &&
        (g_timerLuaRefs[i] != LUA_NOREF)) {
      entry.callback = lua_timer_callback;
      entry.userData = nullptr;
    }
  }
}

/// Registers a Lua timer callback in the current world's timer manager.
runtime::TimerId register_lua_timer(lua_State *state, float seconds,
                                    bool repeat) noexcept {
  if (runtime_binding().world == nullptr) {
    return runtime::kInvalidTimerId;
  }

  ensure_timer_refs_init();
  auto &timerManager = runtime_binding().world->timer_manager();
  const runtime::TimerId id =
      repeat ? timerManager.set_interval(seconds, lua_timer_callback, nullptr)
             : timerManager.set_timeout(seconds, lua_timer_callback, nullptr);
  if (id == runtime::kInvalidTimerId) {
    return id;
  }

  const std::size_t slot = timerManager.slot_for_id(id);
  if (slot >= kMaxTimerRefs) {
    timerManager.cancel(id);
    return runtime::kInvalidTimerId;
  }

  lua_State *refState = timer_ref_state(state);
  if ((refState != nullptr) && (g_timerLuaRefs[slot] != LUA_NOREF)) {
    luaL_unref(refState, LUA_REGISTRYINDEX, g_timerLuaRefs[slot]);
    g_timerLuaRefs[slot] = LUA_NOREF;
  }

  g_timerLuaState = state;
  lua_pushvalue(state, 1);
  g_timerLuaRefs[slot] = luaL_ref(state, LUA_REGISTRYINDEX);
  return id;
}

} // namespace

int lua_engine_set_timeout(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushnil(state);
    return 1;
  }

  const float seconds = static_cast<float>(lua_tonumber(state, 2));
  const runtime::TimerId id = register_lua_timer(state, seconds, false);
  if (id == runtime::kInvalidTimerId) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

int lua_engine_set_interval(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushnil(state);
    return 1;
  }

  const float seconds = static_cast<float>(lua_tonumber(state, 2));
  const runtime::TimerId id = register_lua_timer(state, seconds, true);
  if (id == runtime::kInvalidTimerId) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

int lua_engine_cancel_timer(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || (runtime_binding().world == nullptr)) {
    return 0;
  }

  const auto id = static_cast<runtime::TimerId>(lua_tointeger(state, 1));
  if (id == runtime::kInvalidTimerId) {
    return 0;
  }

  auto &timerManager = runtime_binding().world->timer_manager();
  const std::size_t slot = timerManager.slot_for_id(id);
  if (slot >= kMaxTimerRefs) {
    return 0;
  }

  timerManager.cancel(id);
  lua_State *refState = timer_ref_state(state);
  if ((refState != nullptr) && (g_timerLuaRefs[slot] != LUA_NOREF)) {
    luaL_unref(refState, LUA_REGISTRYINDEX, g_timerLuaRefs[slot]);
    g_timerLuaRefs[slot] = LUA_NOREF;
  }
  return 0;
}

void clear_lua_timer_bindings(lua_State *fallbackState) noexcept {
  ensure_timer_refs_init();

  lua_State *refState = timer_ref_state(fallbackState);
  if (refState != nullptr) {
    for (std::size_t i = 0U; i < kMaxTimerRefs; ++i) {
      if (g_timerLuaRefs[i] != LUA_NOREF) {
        luaL_unref(refState, LUA_REGISTRYINDEX, g_timerLuaRefs[i]);
        g_timerLuaRefs[i] = LUA_NOREF;
      }
    }
  }

  if (runtime_binding().world != nullptr) {
    runtime_binding().world->timer_manager().clear();
  }
  g_timerLuaState = nullptr;
}

void tick_lua_timers(lua_State *state, float deltaSeconds) noexcept {
  if ((state == nullptr) || (runtime_binding().world == nullptr)) {
    return;
  }

  g_timerLuaState = state;
  ensure_timer_refs_init();
  rewire_lua_timer_callbacks();
  runtime_binding().world->timer_manager().tick(deltaSeconds);
}

} // namespace engine::scripting
