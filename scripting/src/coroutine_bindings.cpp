// Implements private Lua coroutine bindings for the scripting module.

#include "coroutine_bindings.h"

#include "binding_util.h"
#include "debug_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>

namespace engine::scripting {
namespace {

/// Identifies how a yielded coroutine decides when to resume.
enum class WaitMode : std::uint8_t {
  Time,
  Condition,
  Frames,
};

/// Stores the Lua registry refs and wake criteria for one coroutine.
struct CoroutineEntry final {
  lua_State *thread = nullptr;
  int threadRef = LUA_NOREF;
  int conditionRef = LUA_NOREF;
  float wakeAt = 0.0F;
  std::uint32_t wakeAtFrame = 0U;
  WaitMode mode = WaitMode::Time;
  bool active = false;
};

char kWaitFramesTag;
char kWaitConditionTag;

/// Owns pending Lua coroutine entries for the scripting module.
class CoroutineScheduler final {
public:
  static constexpr std::size_t kCapacity = 32U;

  /// Parses yield values and stores the next wake criteria. The wait_until
  /// condition is ref'd under protection (this runs from the C-context
  /// scheduler tick where a raising luaL_ref would reach the panic
  /// handler); on ref failure the entry degrades to an immediate timed
  /// wake.
  void parse_yield(lua_State *state, lua_State *thread, int nresults,
                   CoroutineEntry &entry, float totalSeconds,
                   std::uint32_t frameIndex) noexcept {
    entry.mode = WaitMode::Time;
    entry.wakeAt = totalSeconds;
    entry.wakeAtFrame = 0U;
    if ((entry.conditionRef != LUA_NOREF) && (state != nullptr)) {
      luaL_unref(state, LUA_REGISTRYINDEX, entry.conditionRef);
      entry.conditionRef = LUA_NOREF;
    }

    if ((nresults >= 2) && (lua_islightuserdata(thread, -1) != 0)) {
      void *tag = lua_touserdata(thread, -1);
      if (tag == static_cast<void *>(&kWaitFramesTag)) {
        const auto frames =
            static_cast<std::uint32_t>(lua_tointeger(thread, -2));
        entry.mode = WaitMode::Frames;
        entry.wakeAtFrame = frameIndex + frames;
      } else if (tag == static_cast<void *>(&kWaitConditionTag)) {
        lua_pushvalue(thread, -2);
        if (protected_registry_ref(thread, &entry.conditionRef,
                                   "wait_until condition ref")) {
          entry.mode = WaitMode::Condition;
        }
      }
      lua_pop(thread, nresults);
    } else if ((nresults >= 1) && (lua_isnumber(thread, -1) != 0)) {
      const float secs = static_cast<float>(lua_tonumber(thread, -1));
      entry.wakeAt = totalSeconds + secs;
      lua_pop(thread, nresults);
    } else if (nresults > 0) {
      lua_pop(thread, nresults);
    }
  }

  /// Returns true when a wait-until condition allows the coroutine to resume.
  bool check_condition(lua_State *state, int condRef,
                       CoroutineLogLuaErrorFn logLuaError) noexcept {
    if ((state == nullptr) || (condRef == LUA_NOREF)) {
      return false;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, condRef);
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
      if (logLuaError != nullptr) {
        logLuaError(state, "wait_until condition");
      } else {
        lua_pop(state, 1);
      }
      return true;
    }
    const bool result = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return result;
  }

  /// Returns true when the coroutine should resume this tick.
  bool should_wake(lua_State *state, const CoroutineEntry &entry,
                   float totalSeconds, std::uint32_t frameIndex,
                   CoroutineLogLuaErrorFn logLuaError) noexcept {
    switch (entry.mode) {
    case WaitMode::Time:
      return totalSeconds >= entry.wakeAt;
    case WaitMode::Frames:
      return frameIndex >= entry.wakeAtFrame;
    case WaitMode::Condition:
      return check_condition(state, entry.conditionRef, logLuaError);
    }
    return false;
  }

  /// Releases a single coroutine entry and clears its registry refs.
  void release_entry(lua_State *state, CoroutineEntry &entry) noexcept {
    if ((entry.conditionRef != LUA_NOREF) && (state != nullptr)) {
      luaL_unref(state, LUA_REGISTRYINDEX, entry.conditionRef);
    }
    if ((entry.threadRef != LUA_NOREF) && (state != nullptr)) {
      luaL_unref(state, LUA_REGISTRYINDEX, entry.threadRef);
    }
    entry = CoroutineEntry{};
  }

  CoroutineEntry m_entries[kCapacity]{};
};

CoroutineScheduler g_coroutineScheduler;

// #115b: Lua's lua_newthread (the C function behind coroutine.create)
// already copies the creating thread's hookmask/hook/basehookcount into
// the new thread, so a raw coroutine created while the shared debug/
// sandbox hook is armed is not fully unhooked. The gap that survives that
// inheritance is staleness: a coroutine created before sandboxing turns on
// (or before an instruction limit is set) freezes that no-hook state
// forever, since nothing re-applies the CURRENT hook configuration to it
// on a later resume the way engine.start_coroutine's tracked coroutines
// get via refreshLuaHook before every resume. Wrapping both create (belt)
// and resume (suspenders, and the one that actually closes the staleness
// gap) gives raw coroutines the identical before-every-resume guarantee.

/// C replacement for coroutine.create: creates the thread via the real
/// implementation captured as upvalue 1, then arms the shared hook on it
/// so a coroutine that is resumed without ever going through the wrapped
/// resume below (e.g. lua_resume called directly from other C code) still
/// starts with a correct hook rather than whatever it inherited.
int lua_coroutine_create_hooked(lua_State *state) noexcept {
  lua_pushvalue(state, lua_upvalueindex(1));
  lua_pushvalue(state, 1);
  lua_call(state, 1, 1);
  if (lua_isthread(state, -1) != 0) {
    arm_debug_lua_hook(lua_tothread(state, -1));
  }
  return 1;
}

/// C replacement for coroutine.resume: re-arms the shared hook on the
/// target thread with the CURRENT sandbox/debug configuration immediately
/// before resuming, then forwards every argument and result. This is what
/// actually closes the staleness gap above — engine.start_coroutine's
/// tracked coroutines already get this before every resume
/// (tick_lua_coroutines); raw ones did not.
int lua_coroutine_resume_hooked(lua_State *state) noexcept {
  const int nargs = lua_gettop(state);
  if (lua_isthread(state, 1) != 0) {
    arm_debug_lua_hook(lua_tothread(state, 1));
  }
  lua_pushvalue(state, lua_upvalueindex(1));
  lua_insert(state, 1);
  lua_call(state, nargs, LUA_MULTRET);
  return lua_gettop(state);
}

} // namespace

int lua_engine_wait(lua_State *state) noexcept {
  const float secs = (lua_isnumber(state, 1) != 0)
                         ? static_cast<float>(lua_tonumber(state, 1))
                         : 0.0F;
  lua_pushnumber(state, static_cast<lua_Number>(secs));
  return lua_yield(state, 1);
}

int lua_engine_wait_frames(lua_State *state) noexcept {
  const int n =
      (lua_isinteger(state, 1) != 0) ? static_cast<int>(lua_tointeger(state, 1))
                                     : 1;
  lua_pushinteger(state, static_cast<lua_Integer>(n > 0 ? n : 1));
  lua_pushlightuserdata(state, static_cast<void *>(&kWaitFramesTag));
  return lua_yield(state, 2);
}

int lua_engine_wait_until(lua_State *state) noexcept {
  if (lua_isfunction(state, 1) == 0) {
    return luaL_error(state, "wait_until expects a function");
  }
  lua_pushvalue(state, 1);
  lua_pushlightuserdata(state, static_cast<void *>(&kWaitConditionTag));
  return lua_yield(state, 2);
}

int start_lua_coroutine(lua_State *state, float totalSeconds,
                        std::uint32_t frameIndex,
                        CoroutineLogLuaErrorFn logLuaError,
                        CoroutineRefreshHookFn refreshLuaHook) noexcept {
  if (lua_isfunction(state, 1) == 0) {
    lua_pushnil(state);
    return 1;
  }
  for (std::size_t i = 0U; i < CoroutineScheduler::kCapacity; ++i) {
    auto &entry = g_coroutineScheduler.m_entries[i];
    if (entry.active) {
      continue;
    }

    lua_State *thread = lua_newthread(state);
    if (thread == nullptr) {
      lua_pushnil(state);
      return 1;
    }
    const int threadRef = luaL_ref(state, LUA_REGISTRYINDEX);

    lua_pushvalue(state, 1);
    lua_xmove(state, thread, 1);

    if (refreshLuaHook != nullptr) {
      refreshLuaHook(thread);
    }
    int nresults = 0;
    const int status = lua_resume(thread, state, 0, &nresults);
    if (status == LUA_OK) {
      luaL_unref(state, LUA_REGISTRYINDEX, threadRef);
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }
    if (status == LUA_YIELD) {
      entry.thread = thread;
      entry.threadRef = threadRef;
      entry.active = true;
      g_coroutineScheduler.parse_yield(state, thread, nresults, entry,
                                       totalSeconds, frameIndex);
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }

    luaL_unref(state, LUA_REGISTRYINDEX, threadRef);
    if (refreshLuaHook != nullptr) {
      refreshLuaHook(state);
    }
    if (lua_isstring(thread, -1) != 0) {
      lua_xmove(thread, state, 1);
    } else {
      lua_pushstring(state, "start_coroutine error (non-string)");
    }
    if (logLuaError != nullptr) {
      logLuaError(state, "start_coroutine");
    } else {
      lua_pop(state, 1);
    }
    lua_pushnil(state);
    return 1;
  }
  lua_pushnil(state);
  return 1;
}

void tick_lua_coroutines(lua_State *state, float totalSeconds,
                         std::uint32_t frameIndex,
                         CoroutineLogLuaErrorFn logLuaError,
                         CoroutineRefreshHookFn refreshLuaHook) noexcept {
  if (state == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < CoroutineScheduler::kCapacity; ++i) {
    auto &entry = g_coroutineScheduler.m_entries[i];
    if (!entry.active) {
      continue;
    }
    if (!g_coroutineScheduler.should_wake(state, entry, totalSeconds,
                                          frameIndex, logLuaError)) {
      continue;
    }
    if (entry.conditionRef != LUA_NOREF) {
      luaL_unref(state, LUA_REGISTRYINDEX, entry.conditionRef);
      entry.conditionRef = LUA_NOREF;
    }
    if (refreshLuaHook != nullptr) {
      refreshLuaHook(entry.thread);
    }

    int nresults = 0;
    const int status = lua_resume(entry.thread, state, 0, &nresults);
    if (status == LUA_OK) {
      g_coroutineScheduler.release_entry(state, entry);
    } else if (status == LUA_YIELD) {
      g_coroutineScheduler.parse_yield(state, entry.thread, nresults, entry,
                                       totalSeconds, frameIndex);
    } else {
      if (lua_isstring(entry.thread, -1) != 0) {
        lua_xmove(entry.thread, state, 1);
      } else {
        lua_pushstring(state, "coroutine error (non-string)");
      }
      if (logLuaError != nullptr) {
        logLuaError(state, "coroutine");
      } else {
        lua_pop(state, 1);
      }
      g_coroutineScheduler.release_entry(state, entry);
    }
  }
}

void clear_lua_coroutines(lua_State *state) noexcept {
  for (std::size_t i = 0U; i < CoroutineScheduler::kCapacity; ++i) {
    g_coroutineScheduler.release_entry(state, g_coroutineScheduler.m_entries[i]);
  }
}

void install_hooked_coroutine_library(lua_State *state) noexcept {
  lua_getfield(state, -1, "create");
  if (lua_iscfunction(state, -1) == 0) {
    lua_pop(state, 1);
    return;
  }
  lua_pushcclosure(state, &lua_coroutine_create_hooked, 1);
  lua_setfield(state, -2, "create");

  lua_getfield(state, -1, "resume");
  if (lua_iscfunction(state, -1) == 0) {
    lua_pop(state, 1);
    return;
  }
  lua_pushcclosure(state, &lua_coroutine_resume_hooked, 1);
  lua_setfield(state, -2, "resume");

  // coroutine.wrap hides its underlying thread inside the closure it
  // returns, so there is no C-API way to hook it after the fact; rebuild
  // it in Lua on top of the now-hooked create so a wrapped coroutine's
  // thread is hooked too. error(results[2], 0) forwards the resume error
  // object (string or not) unchanged, matching the stock implementation's
  // no-extra-position-info behavior.
  constexpr const char *kWrapShim =
      "return function(create)\n"
      "  return function(f)\n"
      "    local co = create(f)\n"
      "    return function(...)\n"
      "      local results = { coroutine.resume(co, ...) }\n"
      "      if not results[1] then error(results[2], 0) end\n"
      "      return table.unpack(results, 2)\n"
      "    end\n"
      "  end\n"
      "end\n";
  if ((luaL_loadstring(state, kWrapShim) != LUA_OK) ||
      (lua_pcall(state, 0, 1, 0) != LUA_OK)) {
    lua_pop(state, 1);
    return;
  }
  lua_getfield(state, -2, "create");
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    lua_pop(state, 1);
    return;
  }
  lua_setfield(state, -2, "wrap");
}

} // namespace engine::scripting
