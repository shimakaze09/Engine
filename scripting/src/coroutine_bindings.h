// Declares private Lua coroutine bindings for the scripting module.

#pragma once

#include <cstdint>

struct lua_State;

namespace engine::scripting {

using CoroutineLogLuaErrorFn = void (*)(lua_State *state,
                                        const char *context) noexcept;
using CoroutineRefreshHookFn = void (*)(lua_State *thread) noexcept;

/// Lua binding: Lua engine.wait(seconds).
int lua_engine_wait(lua_State *state) noexcept;
/// Lua binding: Lua engine.wait_frames(frame_count).
int lua_engine_wait_frames(lua_State *state) noexcept;
/// Lua binding: Lua engine.wait_until(callback).
int lua_engine_wait_until(lua_State *state) noexcept;

/// Starts a Lua coroutine using the supplied scheduler clock; the refresh
/// hook arms sandbox/debug hooks on the new thread before its first resume.
int start_lua_coroutine(lua_State *state, float totalSeconds,
                        std::uint32_t frameIndex,
                        CoroutineLogLuaErrorFn logLuaError,
                        CoroutineRefreshHookFn refreshLuaHook) noexcept;

/// Advances active Lua coroutines against the supplied scheduler clock.
void tick_lua_coroutines(lua_State *state, float totalSeconds,
                         std::uint32_t frameIndex,
                         CoroutineLogLuaErrorFn logLuaError,
                         CoroutineRefreshHookFn refreshLuaHook) noexcept;

/// Releases all active Lua coroutine registry refs.
void clear_lua_coroutines(lua_State *state) noexcept;

/// Replaces coroutine.create and coroutine.resume (and reimplements
/// coroutine.wrap on top of the replacements) so every Lua thread is armed
/// with the CURRENT shared debug/sandbox hook before every resume, not just
/// ones started through engine.start_coroutine (issue #115b). Lua's
/// lua_newthread already copies hook state from the creating thread, so the
/// gap the resume wrapper closes is staleness: a thread created before
/// sandboxing was configured (or before a later reconfiguration) otherwise
/// keeps running with whatever hook state it was born with. Call once,
/// immediately after the coroutine library loads, with that library's
/// table on top of the stack; the table is left on top on return, matching
/// luaL_requiref's convention of leaving the pop to the caller.
void install_hooked_coroutine_library(lua_State *state) noexcept;

} // namespace engine::scripting
