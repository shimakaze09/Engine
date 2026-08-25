// Declares private Lua entity script module cache bindings.

#pragma once

#include <cstdint>

#include "engine/core/entity.h"
#include "engine/runtime/world.h"

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

/// Clears the Lua state alias and callbacks installed by configure. Called
/// by the VM's owner as it destroys the VM: every entry point here guards
/// on the alias, so clearing it turns a post-shutdown call into a no-op
/// instead of a dispatch into the freed lua_State. Distinct from
/// reset_entity_script_bindings, which clears run-scoped cache state while
/// the VM stays alive.
void clear_entity_script_bindings() noexcept;

/// Lua binding: Lua engine.require(path).
int lua_engine_require(lua_State *state) noexcept;

/// Clears cached entity script modules and hot-reload state.
void reset_entity_script_bindings() noexcept;

/// True while an entity on_end_play callback is executing; world mutations
/// must queue during dispatch so destruction cannot reenter mid-iteration.
bool in_end_play_dispatch() noexcept;

/// Fires on_end_play for the entity and its alive transform subtree
/// (descendants first) for members that received begin_play; called before
/// script-initiated immediate destroys.
void dispatch_entity_subtree_end_play(runtime::World *world,
                                      runtime::Entity entity) noexcept;

} // namespace engine::scripting
