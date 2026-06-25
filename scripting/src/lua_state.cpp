// Implements private Lua state ownership for the scripting module.

#include "lua_state.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace engine::scripting {
namespace {
ScriptingContext g_scriptingContext{};
} // namespace

/// Returns the process-local scripting context.
ScriptingContext &scripting_context() noexcept { return g_scriptingContext; }

/// Creates the Lua state if needed and returns the current state.
lua_State *initialize_lua_state() noexcept {
  ScriptingContext &context = scripting_context();
  if (context.luaState == nullptr) {
    context.luaState = luaL_newstate();
  }
  return context.luaState;
}

/// Returns the current Lua state, or nullptr when scripting is shut down.
lua_State *current_lua_state() noexcept { return scripting_context().luaState; }

/// Closes and clears the current Lua state.
void shutdown_lua_state() noexcept {
  ScriptingContext &context = scripting_context();
  if (context.luaState != nullptr) {
    lua_close(context.luaState);
    context.luaState = nullptr;
  }
}

} // namespace engine::scripting
