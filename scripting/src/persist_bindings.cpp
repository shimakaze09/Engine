// Owns Lua persist table bindings for the Engine scripting system.

#include "persist_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace engine::scripting {
namespace {

int g_persistRef = LUA_NOREF;

} // namespace

int lua_engine_persist(lua_State *state) noexcept {
  const char *key = luaL_checkstring(state, 1);
  if (g_persistRef == LUA_NOREF) {
    lua_newtable(state);
    g_persistRef = luaL_ref(state, LUA_REGISTRYINDEX);
  }

  lua_rawgeti(state, LUA_REGISTRYINDEX, g_persistRef);
  lua_pushvalue(state, 2);
  lua_setfield(state, -2, key);
  lua_pop(state, 1);
  return 0;
}

int lua_engine_restore(lua_State *state) noexcept {
  const char *key = luaL_checkstring(state, 1);
  if (g_persistRef == LUA_NOREF) {
    lua_pushnil(state);
    return 1;
  }

  lua_rawgeti(state, LUA_REGISTRYINDEX, g_persistRef);
  lua_getfield(state, -1, key);
  lua_remove(state, -2);
  return 1;
}

void clear_persist_bindings(lua_State *state) noexcept {
  if ((state != nullptr) && (g_persistRef != LUA_NOREF)) {
    luaL_unref(state, LUA_REGISTRYINDEX, g_persistRef);
  }
  g_persistRef = LUA_NOREF;
}

} // namespace engine::scripting
