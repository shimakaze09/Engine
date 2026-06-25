// Declares private Lua coroutine bindings for the scripting module.

#pragma once

#include <cstdint>

struct lua_State;

namespace engine::scripting {

using CoroutineLogLuaErrorFn = void (*)(const char *context) noexcept;
using CoroutineRefreshHookFn = void (*)() noexcept;

/// Handles Lua engine.wait(seconds).
int lua_engine_wait(lua_State *state) noexcept;
/// Handles Lua engine.wait_frames(frame_count).
int lua_engine_wait_frames(lua_State *state) noexcept;
/// Handles Lua engine.wait_until(callback).
int lua_engine_wait_until(lua_State *state) noexcept;

/// Starts a Lua coroutine using the supplied scheduler clock.
int start_lua_coroutine(lua_State *state, float totalSeconds,
                        std::uint32_t frameIndex,
                        CoroutineLogLuaErrorFn logLuaError) noexcept;

/// Advances active Lua coroutines against the supplied scheduler clock.
void tick_lua_coroutines(lua_State *state, float totalSeconds,
                         std::uint32_t frameIndex,
                         CoroutineLogLuaErrorFn logLuaError,
                         CoroutineRefreshHookFn refreshLuaHook) noexcept;

/// Releases all active Lua coroutine registry refs.
void clear_lua_coroutines(lua_State *state) noexcept;

} // namespace engine::scripting
