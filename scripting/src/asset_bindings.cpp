// Implements asset and prefab Lua bindings (prefab save/instantiate, async
// asset loads, script components)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "asset_bindings.h"

#include "binding_util.h"
#include "deferred_mutations.h"
#include "entity_handle.h"
#include "lua_state.h"
#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/quat.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"

namespace engine::scripting {

namespace {

int lua_engine_save_prefab(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !lua_isinteger(state, 1) ||
      !lua_isstring(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 2);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok =
      (runtime_binding().services != nullptr) && (runtime_binding().services->save_prefab != nullptr)
          ? runtime_binding().services->save_prefab(runtime_binding().world, entity.index, path)
          : false;
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_instantiate(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  const std::uint32_t entityIndex =
      ((runtime_binding().services != nullptr) && (runtime_binding().services->instantiate_prefab != nullptr))
          ? runtime_binding().services->instantiate_prefab(runtime_binding().world, path)
          : 0U;
  if (entityIndex == 0U) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle_from_index(state, entityIndex);
  return 1;
}

// --- Async asset streaming (P1-M4-C2c) ---

// engine.load_asset_async(path [, priority]) → handle_index or nil
// priority: 0=Low, 1=Normal, 2=High, 3=Immediate (default=Normal)
int lua_engine_load_asset_async(lua_State *state) noexcept {
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->load_asset_async == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  if (!lua_isstring(state, 1)) {
    luaL_traceback(state, state, "load_asset_async: path must be a string", 1);
    core::log_message(core::LogLevel::Error, "scripting",
                      lua_tostring(state, -1));
    lua_pop(state, 1);
    lua_pushnil(state);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  std::uint8_t priority = 1U; // Normal
  if (lua_isinteger(state, 2)) {
    const lua_Integer p = lua_tointeger(state, 2);
    if ((p >= 0) && (p <= 3)) {
      priority = static_cast<std::uint8_t>(p);
    }
  }
  const std::uint32_t handle = runtime_binding().services->load_asset_async(path, priority);
  if (handle == 0xFFFFFFFFU) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(handle));
  return 1;
}

// engine.is_asset_ready(handle_index) → boolean
int lua_engine_is_asset_ready(lua_State *state) noexcept {
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->is_asset_ready == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isinteger(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const std::uint32_t handleIndex =
      static_cast<std::uint32_t>(lua_tointeger(state, 1));
  lua_pushboolean(state, runtime_binding().services->is_asset_ready(handleIndex) ? 1 : 0);
  return 1;
}

int lua_engine_add_script_component(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *path = lua_tostring(state, 2);
  if ((path == nullptr) || (path[0] == '\0')) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::ScriptComponent comp{};
  const std::size_t maxLen = sizeof(comp.scriptPath) - 1U;
  const std::size_t len = std::strlen(path);
  const std::size_t copy = (len > maxLen) ? maxLen : len;
  std::memcpy(comp.scriptPath, path, copy);
  comp.scriptPath[copy] = '\0';

  lua_pushboolean(state, apply_or_queue_script_component(entity, comp) ? 1 : 0);
  return 1;
}

int lua_engine_remove_script_component(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state,
                  apply_or_queue_remove_script_component(entity) ? 1 : 0);
  return 1;
}

} // namespace

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_asset_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_save_prefab);
  lua_setfield(state, -2, "save_prefab");
  lua_pushcfunction(state, &lua_engine_instantiate);
  lua_setfield(state, -2, "instantiate");
  lua_pushcfunction(state, &lua_engine_load_asset_async);
  lua_setfield(state, -2, "load_asset_async");
  lua_pushcfunction(state, &lua_engine_is_asset_ready);
  lua_setfield(state, -2, "is_asset_ready");
  lua_pushcfunction(state, &lua_engine_add_script_component);
  lua_setfield(state, -2, "add_script_component");
  lua_pushcfunction(state, &lua_engine_remove_script_component);
  lua_setfield(state, -2, "remove_script_component");
}

} // namespace engine::scripting
