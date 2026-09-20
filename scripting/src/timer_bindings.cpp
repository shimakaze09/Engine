// Owns Lua timer bindings for the Engine scripting system.

#include "timer_bindings.h"

#include "binding_util.h"
#include "lua_state.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>

#include "reload_transaction.h"
#include "runtime_binding.h"

namespace engine::scripting {
namespace {

/// Timer ids are opaque bridge values; zero never names a timer.
using TimerId = std::uint32_t;
constexpr TimerId kInvalidTimerId = 0U;
constexpr std::size_t kMaxTimerRefs = kMaxTimerSlots;
/// Owns one Lua callback registry ref for an exact timer generation.
struct LuaTimerRef final {
  TimerId ownerId = kInvalidTimerId;
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
void lua_timer_callback(TimerId id, void *userData) noexcept {
  (void)userData;
  lua_State *const mainState = timer_main_state(nullptr);
  if ((mainState == nullptr) || (id == kInvalidTimerId) || !runtime_bound()) {
    return;
  }

  const RuntimeServices &services = *runtime_binding().services;
  runtime::World *const world = runtime_binding().world;
  const std::size_t slot = services.timer_slot_for_id(world, id);
  bool wasRepeating = false;
  bool wasActive = false;
  if ((slot >= kMaxTimerRefs) ||
      !services.timer_slot_state(world, slot, &wasRepeating, &wasActive)) {
    return;
  }

  const LuaTimerRef firedRef = g_timerLuaRefs[slot];
  if ((firedRef.ownerId != id) || (firedRef.registryRef == LUA_NOREF)) {
    return;
  }

  TimerCallArgs args{};
  args.registryRef = firedRef.registryRef;
  static_cast<void>(protected_engine_dispatch(
      mainState, &timer_call_trampoline, &args, 0, "timer"));

  if (!runtime_bound()) {
    return;
  }

  bool repeatNow = false;
  bool activeNow = false;
  const bool stillCurrent =
      (services.timer_slot_for_id(world, id) == slot) &&
      services.timer_slot_state(world, slot, &repeatNow, &activeNow) &&
      activeNow;
  if (!wasRepeating || !stillCurrent) {
    LuaTimerRef &currentRef = g_timerLuaRefs[slot];
    if (currentRef.ownerId == id) {
      release_timer_ref(currentRef, mainState);
    }
  }
}

/// Registers a Lua timer callback in the current world's timer manager.
TimerId register_lua_timer(lua_State *state, float seconds,
                                    bool repeat) noexcept {
  if (!runtime_bound() ||
      (reload_staging(ReloadEffect::TimerCreate) == ReloadStaging::Refused)) {
    return kInvalidTimerId;
  }

  ensure_timer_refs_init();
  const RuntimeServices &services = *runtime_binding().services;
  runtime::World *const world = runtime_binding().world;
  const TimerId id =
      services.timer_set(world, seconds, repeat, &lua_timer_callback, nullptr);
  if (id == kInvalidTimerId) {
    return id;
  }

  const std::size_t slot = services.timer_slot_for_id(world, id);
  if (slot >= kMaxTimerRefs) {
    services.timer_cancel(world, id);
    return kInvalidTimerId;
  }

  release_timer_ref(g_timerLuaRefs[slot], timer_main_state(state));
  reload_note_timer_created(id);

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
  const TimerId id = register_lua_timer(state, seconds, false);
  if (id == kInvalidTimerId) {
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
  const TimerId id = register_lua_timer(state, seconds, true);
  if (id == kInvalidTimerId) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

int lua_engine_cancel_timer(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || !runtime_bound()) {
    return 0;
  }

  const auto id = static_cast<TimerId>(lua_tointeger(state, 1));
  if (id == kInvalidTimerId) {
    return 0;
  }
  switch (reload_staging(ReloadEffect::TimerCancel)) {
  case ReloadStaging::None:
    cancel_lua_timer(id);
    break;
  case ReloadStaging::Staged:
    reload_hold_timer_cancel(id);
    break;
  case ReloadStaging::Refused:
    core::log_message(core::LogLevel::Warning, "scripting",
                      "cancel_timer refused: the hot reload holds no more");
    break;
  }
  return 0;
}

void cancel_lua_timer(std::uint32_t timerId) noexcept {
  if ((timerId == kInvalidTimerId) || !runtime_bound()) {
    return;
  }
  const RuntimeServices &services = *runtime_binding().services;
  runtime::World *const world = runtime_binding().world;
  const std::size_t slot = services.timer_slot_for_id(world, timerId);
  if (slot >= kMaxTimerRefs) {
    return;
  }

  services.timer_cancel(world, timerId);
  lua_State *refState = timer_main_state(nullptr);
  LuaTimerRef &timerRef = g_timerLuaRefs[slot];
  if (timerRef.ownerId == timerId) {
    release_timer_ref(timerRef, refState);
  }
}

void clear_lua_timer_bindings(lua_State *fallbackState) noexcept {
  ensure_timer_refs_init();

  lua_State *refState = timer_main_state(fallbackState);
  for (auto &timerRef : g_timerLuaRefs) {
    release_timer_ref(timerRef, refState);
  }

  if (runtime_bound()) {
    runtime_binding().services->timer_clear(runtime_binding().world);
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
  if ((state == nullptr) || !runtime_bound()) {
    return;
  }

  ensure_timer_refs_init();
  static_cast<void>(
      runtime_binding().services->timer_tick(runtime_binding().world, deltaSeconds));
}

} // namespace engine::scripting
