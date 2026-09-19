// Owns Lua timer bindings for the Engine scripting system.

#include "timer_bindings.h"

#include "binding_util.h"
#include "lua_state.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>

#include "engine/runtime/timer_manager.h"
#include "engine/runtime/world.h"
#include "runtime_binding.h"

namespace engine::scripting {
namespace {

constexpr std::size_t kMaxTimerRefs = runtime::TimerManager::kMaxTimers;
/// Owns one Lua callback registry ref for an exact timer generation.
struct LuaTimerRef final {
  runtime::TimerId ownerId = runtime::kInvalidTimerId;
  int registryRef = LUA_NOREF;
};

LuaTimerRef g_timerLuaRefs[kMaxTimerRefs];
bool g_timerRefsInit = false;

/// Initializes Lua timer reference storage on first use.
void ensure_timer_refs_init() noexcept {
  if (g_timerRefsInit) {
    return;
  }

  for (auto &timerRef : g_timerLuaRefs) {
    timerRef = LuaTimerRef{};
  }
  g_timerRefsInit = true;
}

/// Returns the state timer refs are released on and callbacks dispatched
/// on. Never the calling thread.
///
/// `set_timeout` and `set_interval` are reachable from inside a coroutine —
/// `start_lua_coroutine` resumes immediately, so a binding called there
/// receives the coroutine's thread — and that thread may be suspended, or
/// finished and collected, by the time the timer fires or its ref is
/// released. Registry refs are VM-global, so the main state can release any
/// of them, and dispatching there never resumes a suspended coroutine's
/// stack. The argument is used only before the VM records a main state.
lua_State *timer_main_state(lua_State *fallbackState) noexcept {
  lua_State *mainState = current_lua_state();
  return (mainState != nullptr) ? mainState : fallbackState;
}

/// Releases one Lua timer callback ref and clears its timer ownership.
void release_timer_ref(LuaTimerRef &timerRef, lua_State *state) noexcept {
  if ((state != nullptr) && (timerRef.registryRef != LUA_NOREF)) {
    luaL_unref(state, LUA_REGISTRYINDEX, timerRef.registryRef);
  }
  timerRef = LuaTimerRef{};
}

/// Carries one fired timer callback ref into the protected trampoline.
struct TimerCallArgs final {
  int registryRef = LUA_NOREF;
};

/// Protected trampoline: resolves the timer callback ref and calls it.
int timer_call_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<TimerCallArgs *>(lua_touserdata(state, 1));
  lua_rawgeti(state, LUA_REGISTRYINDEX, args->registryRef);
  if (lua_isfunction(state, -1) == 0) {
    return 0;
  }
  lua_call(state, 0, 0);
  return 0;
}

/// Invokes a Lua callback for a fired runtime timer.
void lua_timer_callback(runtime::TimerId id, void *userData) noexcept {
  (void)userData;
  lua_State *const mainState = timer_main_state(nullptr);
  if ((mainState == nullptr) || (id == runtime::kInvalidTimerId) ||
      (runtime_binding().world == nullptr)) {
    return;
  }

  auto &timerManager = runtime_binding().world->timer_manager();
  const std::size_t slot = timerManager.slot_for_id(id);
  if (slot >= kMaxTimerRefs) {
    return;
  }

  const bool wasRepeating = timerManager.entry_at(slot).repeat;
  const LuaTimerRef firedRef = g_timerLuaRefs[slot];
  if ((firedRef.ownerId != id) || (firedRef.registryRef == LUA_NOREF)) {
    return;
  }

  TimerCallArgs args{};
  args.registryRef = firedRef.registryRef;
  static_cast<void>(protected_engine_dispatch(
      mainState, &timer_call_trampoline, &args, 0, "timer"));

  if (runtime_binding().world == nullptr) {
    return;
  }

  auto &currentTimerManager = runtime_binding().world->timer_manager();
  const bool stillCurrent =
      (currentTimerManager.slot_for_id(id) == slot) &&
      currentTimerManager.entry_at(slot).active;
  if (!wasRepeating || !stillCurrent) {
    LuaTimerRef &currentRef = g_timerLuaRefs[slot];
    if (currentRef.ownerId == id) {
      release_timer_ref(currentRef, mainState);
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

  release_timer_ref(g_timerLuaRefs[slot], timer_main_state(state));

  // The callback is on the calling thread's stack, so the ref is taken
  // there; the resulting registry ref is VM-global and is released from
  // the main state.
  lua_pushvalue(state, 1);
  g_timerLuaRefs[slot].ownerId = id;
  g_timerLuaRefs[slot].registryRef = luaL_ref(state, LUA_REGISTRYINDEX);
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
  lua_State *refState = timer_main_state(state);
  LuaTimerRef &timerRef = g_timerLuaRefs[slot];
  if (timerRef.ownerId == id) {
    release_timer_ref(timerRef, refState);
  }
  return 0;
}

void clear_lua_timer_bindings(lua_State *fallbackState) noexcept {
  ensure_timer_refs_init();

  lua_State *refState = timer_main_state(fallbackState);
  for (auto &timerRef : g_timerLuaRefs) {
    release_timer_ref(timerRef, refState);
  }

  if (runtime_binding().world != nullptr) {
    runtime_binding().world->timer_manager().clear();
  }
}

std::size_t active_lua_timer_ref_count() noexcept {
  ensure_timer_refs_init();
  std::size_t count = 0U;
  for (const auto &timerRef : g_timerLuaRefs) {
    if (timerRef.registryRef != LUA_NOREF) {
      ++count;
    }
  }
  return count;
}

void tick_lua_timers(lua_State *state, float deltaSeconds) noexcept {
  if ((state == nullptr) || (runtime_binding().world == nullptr)) {
    return;
  }

  ensure_timer_refs_init();
  runtime_binding().world->timer_manager().tick(deltaSeconds);
}

} // namespace engine::scripting
