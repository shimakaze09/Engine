// Declares private Lua state ownership for the scripting module.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Owns process-local scripting runtime state.
struct ScriptingContext final {
  lua_State *luaState = nullptr;
};

/// Returns the process-local scripting context.
ScriptingContext &scripting_context() noexcept;

/// Creates the Lua state if needed and returns the current state.
lua_State *initialize_lua_state() noexcept;

/// Returns the current Lua state, or nullptr when scripting is shut down.
lua_State *current_lua_state() noexcept;

/// Closes and clears the current Lua state.
void shutdown_lua_state() noexcept;

} // namespace engine::scripting
