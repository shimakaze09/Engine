// Declares shared Lua binding helpers (argument parsing and error logging)
// used across the scripting module's binding translation units.

#pragma once

struct lua_State;

#include <cstddef>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::scripting {

/// Reads a point, direction or colour starting at argument `*index`: a
/// vec3 value, or three numbers as every binding took before vec3 existed.
/// Advances `*index` past what it read (one argument or three); fails, with
/// `*index` unchanged, on anything else or a non-finite component.
bool read_vec3_arg(lua_State *state, int *index, math::Vec3 *outVec) noexcept;

/// An optional point or direction at `*index`: a vec3 value, or three
/// number slots, each taking its component of `defaults` when absent or
/// nil. Advances `*index` by one or three; fails on a present non-number or
/// non-finite component.
bool read_optional_vec3_arg(lua_State *state, int *index,
                            const math::Vec3 &defaults,
                            math::Vec3 *outVec) noexcept;

/// read_vec3_arg for a rotation: a quat value or four numbers x, y, z, w.
bool read_quat_arg(lua_State *state, int *index, math::Quat *outQuat) noexcept;

/// Reads one finite number arg; fails on a non-number or non-finite value.
bool read_finite_number_arg(lua_State *state, int index,
                            float *outValue) noexcept;

/// Reads an optional finite number arg: an absent or nil arg yields
/// defaultValue, a present arg must be a finite number. Fails on a present
/// non-number or non-finite value so a garbage optional never silently
/// becomes the default.
bool read_optional_finite_number_arg(lua_State *state, int index,
                                     float defaultValue,
                                     float *outValue) noexcept;

/// Copies a NUL-terminated path into a fixed buffer; refuses with one
/// Error diagnostic (destination untouched) when the path does not fit.
bool copy_path_strict(char *dst, std::size_t dstCapacity, const char *src,
                      const char *context) noexcept;

/// True when a script-supplied filesystem path stays inside the VFS jail
/// (relative, no "..", no drive/backslash); refusal logs one Error naming
/// the call site.
bool script_path_in_jail(const char *path, const char *context) noexcept;

/// Resolves a script path to the OS path its chunk is read from: through
/// the VFS mount when the path's prefix is mounted, so scripts load from
/// the configured asset root rather than from wherever the process was
/// launched; a path under no mount keeps its cwd-relative spelling,
/// which is what tests and tooling that write scripts beside the binary
/// rely on. False when the resolved path does not fit outCapacity.
bool resolve_script_os_path(const char *path, char *out,
                            std::size_t outCapacity) noexcept;

/// Logs the Lua error on top of the stack, then pops it. The error carries
/// its call stack when it came from traced_pcall or
/// push_coroutine_traceback, which capture it before the stack unwinds.
void log_lua_error(const char *context) noexcept;

/// Same, but on an explicit stack (e.g. a coroutine thread's caller).
void log_lua_error(lua_State *state, const char *context) noexcept;

/// lua_pcall of the function and `nargs` arguments on top of the stack,
/// with a message handler that appends the call stack to the error while
/// the failing frames still exist; after lua_pcall returns they are gone,
/// so a traceback built later describes only its own caller. Same stack
/// effect as lua_pcall: results, or the traced error, on top.
bool traced_pcall(lua_State *state, int nargs, int nresults) noexcept;

/// Replaces the error on top of a failed coroutine's stack with that
/// error and the coroutine's call stack, pushed on `state`; the failed
/// coroutine keeps its frames, so they are read from it. Leaves a plain
/// message when the traceback cannot be built.
void push_coroutine_traceback(lua_State *state, lua_State *coroutine) noexcept;

/// Signature for protected-dispatch trampolines: a lua_CFunction body that
/// receives its argument struct as a light userdata at stack index 1.
using LuaDispatchFn = int (*)(lua_State *state);

/// Runs an engine-to-Lua dispatch inside one protected call charged against
/// the shared per-frame instruction budget: pushes the
/// trampoline and args (both allocation-free), pcalls with nresults, and
/// logs errors — including a budget exhausted by a latched
/// instruction-limit trip. Returns success with the results on the stack.
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
