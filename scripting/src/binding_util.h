// Declares shared Lua binding helpers (argument parsing and error logging)
// used across the scripting module's binding translation units.

#pragma once

struct lua_State;

#include "engine/math/vec3.h"

namespace engine::scripting {

/// Reads three consecutive number args starting at startIndex into a Vec3.
bool read_vec3_args(lua_State *state, int startIndex,
                    math::Vec3 *outVec) noexcept;

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

} // namespace engine::scripting
