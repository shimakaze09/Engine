// Declares private Lua entity script module cache bindings.

#pragma once

#include <cstdint>

#include "engine/core/entity.h"

struct lua_State;

namespace engine::scripting {

using EntityScriptPushEntityHandleFn =
    void (*)(lua_State *state, core::Entity entity) noexcept;
using EntityScriptLogLuaErrorFn = void (*)(const char *context) noexcept;
using EntityScriptRefreshLuaHookFn = void (*)() noexcept;
using EntityScriptFileMtimeFn = std::int64_t (*)(const char *path) noexcept;

/// Stores callbacks supplied by scripting.cpp for entity script dispatch.
struct EntityScriptBindingCallbacks final {
  EntityScriptPushEntityHandleFn pushEntityHandle = nullptr;
  EntityScriptLogLuaErrorFn logLuaError = nullptr;
  EntityScriptRefreshLuaHookFn refreshLuaHook = nullptr;
  EntityScriptFileMtimeFn fileMtime = nullptr;
};

/// Binds the Lua state and callback hooks used by entity script modules.
void configure_entity_script_bindings(
    lua_State *state, const EntityScriptBindingCallbacks &callbacks) noexcept;

/// Lua binding: Lua engine.require(path).
int lua_engine_require(lua_State *state) noexcept;

/// Clears cached entity script modules and hot-reload state.
void reset_entity_script_bindings() noexcept;

} // namespace engine::scripting
