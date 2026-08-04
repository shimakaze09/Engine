// Declares shared Lua binding helpers (argument parsing and error logging)
// used across the scripting module's binding translation units.

#pragma once

struct lua_State;

#include "engine/math/vec3.h"

namespace engine::scripting {

/// Reads three consecutive finite number args starting at startIndex into a
/// Vec3; fails on a non-number or non-finite component.
bool read_vec3_args(lua_State *state, int startIndex,
                    math::Vec3 *outVec) noexcept;

/// Reads one finite number arg; fails on a non-number or non-finite value.
bool read_finite_number_arg(lua_State *state, int index,
                            float *outValue) noexcept;

/// Logs the Lua error on top of the stack with a traceback, then pops it.
void log_lua_error(const char *context) noexcept;

/// Same, but on an explicit stack (e.g. a coroutine thread's caller).
void log_lua_error(lua_State *state, const char *context) noexcept;

/// Signature for protected-dispatch trampolines: a lua_CFunction body that
/// receives its argument struct as a light userdata at stack index 1.
using LuaDispatchFn = int (*)(lua_State *state);

/// Runs an engine-to-Lua dispatch inside one protected call with a fresh
/// per-dispatch instruction budget: pushes the trampoline and args (both
/// allocation-free), pcalls with nresults, and logs errors — including a
/// budget exhausted by a latched instruction-limit trip. Returns success
/// with the results left on the stack.
bool protected_engine_dispatch(lua_State *state, LuaDispatchFn trampoline,
                               void *args, int nresults,
                               const char *context) noexcept;

/// Runs an allocation-hazardous engine C operation (chunk loads, registry
/// refs, table snapshots) inside one pcall WITHOUT arming a fresh
/// instruction budget — it executes no user Lua code but can still raise
/// LUA_ERRMEM, which is logged and absorbed instead of reaching the panic
/// handler. Returns success with the results left on the stack.
bool protected_c_operation(lua_State *state, LuaDispatchFn trampoline,
                           void *args, int nresults,
                           const char *context) noexcept;

/// Loads a Lua chunk from path under protection (luaL_loadfile can raise
/// LUA_ERRMEM building the chunk name before the protected parse); on
/// success the chunk function is left on the stack, on failure the error
/// is logged and the stack is balanced.
bool protected_load_chunk(lua_State *state, const char *path,
                          const char *context) noexcept;

/// Pops the value on top of the stack and stores it in the registry under
/// protection (luaL_ref can raise LUA_ERRMEM growing the registry). The
/// value is consumed either way; outRef holds LUA_NOREF on failure.
bool protected_registry_ref(lua_State *state, int *outRef,
                            const char *context) noexcept;

} // namespace engine::scripting
