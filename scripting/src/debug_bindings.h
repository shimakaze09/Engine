// Declares private Lua profiler, debugger, and hook-state bindings.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Lua binding: Lua engine.profiler_enable(enabled).
int lua_engine_profiler_enable(lua_State *state) noexcept;
/// Lua binding: Lua engine.profiler_reset().
int lua_engine_profiler_reset(lua_State *state) noexcept;
/// Lua binding: Lua engine.profiler_get_count(name).
int lua_engine_profiler_get_count(lua_State *state) noexcept;
/// Lua binding: Lua engine.debugger_enable(enabled).
int lua_engine_debugger_enable(lua_State *state) noexcept;
/// Lua binding: Lua engine.debugger_add_breakpoint(file, line).
int lua_engine_debugger_add_breakpoint(lua_State *state) noexcept;
/// Lua binding: Lua engine.debugger_clear_breakpoints().
int lua_engine_debugger_clear_breakpoints(lua_State *state) noexcept;
/// Lua binding: Lua engine.debugger_add_watch(expr).
int lua_engine_debugger_add_watch(lua_State *state) noexcept;
/// Lua binding: Lua engine.debugger_clear_watches().
int lua_engine_debugger_clear_watches(lua_State *state) noexcept;
/// Lua binding: Lua engine.debugger_last_breakpoint().
int lua_engine_debugger_last_breakpoint(lua_State *state) noexcept;
/// Lua binding: Lua engine.debugger_last_callstack().
int lua_engine_debugger_last_callstack(lua_State *state) noexcept;
/// Lua binding: Lua engine.debugger_last_watch_values().
int lua_engine_debugger_last_watch_values(lua_State *state) noexcept;

/// Sets the Lua state that owns debugger/profiler hooks.
void set_debug_lua_state(lua_State *state) noexcept;
/// Refreshes debugger/profiler/sandbox hooks on the owning Lua state.
void refresh_debug_lua_hook() noexcept;
/// Arms hooks for an engine-side dispatch boundary; the shared per-frame
/// instruction budget is NOT reset here (issue #84: one budget per frame).
void arm_debug_lua_hook(lua_State *state) noexcept;
/// True once this frame's shared instruction budget is exhausted.
bool debug_instruction_budget_exhausted() noexcept;
/// Refills the shared per-frame instruction budget; called at frame
/// boundaries (set_frame_index) and on limit/sandbox reconfiguration.
void refill_debug_instruction_budget() noexcept;
/// Applies the current hook configuration to an explicit Lua thread; hooks
/// are per-thread in Lua 5.4, so coroutines must arm them before resuming.
void apply_debug_lua_hook(lua_State *state) noexcept;
/// Clears transient debugger/profiler state.
void reset_debug_bindings() noexcept;

/// Clears all registered debugger breakpoints.
void debugger_clear_breakpoints() noexcept;
/// Clears only the breakpoints registered for one source file, matching the
/// DAP setBreakpoints per-source replace contract.
void debugger_clear_breakpoints_for_source(const char *file) noexcept;
/// Adds a debugger breakpoint matched by source suffix and line.
bool debugger_add_breakpoint(const char *file, int line) noexcept;

/// Updates the sandbox-enabled flag used by the debug hook.
void set_debug_sandbox_enabled(bool enabled) noexcept;
/// Returns the sandbox-enabled flag used by the debug hook.
bool debug_sandbox_enabled() noexcept;
/// Updates the instruction limit used by the debug hook.
void set_debug_instruction_limit(int limit) noexcept;
/// Returns the instruction limit used by the debug hook.
int debug_instruction_limit() noexcept;

} // namespace engine::scripting
