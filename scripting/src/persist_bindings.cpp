// Owns Lua persistence bindings for the Engine scripting system: the
// in-memory hot-reload persist table plus the on-disk single-slot save
// (engine.save_data / engine.load_data, flat table <-> JSON).

#include "persist_bindings.h"

#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>

#include "engine/core/json.h"
#include "engine/runtime/scripting_bridge.h"

namespace engine::scripting {
namespace {

int g_persistRef = LUA_NOREF;

constexpr std::size_t kMaxSaveKeys = 64U;
constexpr std::size_t kMaxSaveJsonBytes = 16U * 1024U;

} // namespace

/// Lua binding: engine.persist(key, value) stores value under key in the
/// hot-reload persist table. Calling with no value argument is an error
/// (a forgotten value must not silently delete data); the documented
/// deletion form is an explicit engine.persist(key, nil).
int lua_engine_persist(lua_State *state) noexcept {
  const char *key = luaL_checkstring(state, 1);
  if (lua_gettop(state) < 2) {
    return luaL_error(state,
                      "engine.persist(key, value) requires a value argument; "
                      "pass an explicit nil to delete the key");
  }
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

int lua_engine_save_data(lua_State *state) noexcept {
  if (!lua_istable(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if ((runtime_binding().services == nullptr) ||
      (runtime_binding().services->save_game_data == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  core::JsonWriter writer{};
  writer.begin_object();
  writer.begin_array("entries");
  std::size_t keyCount = 0U;
  bool valid = true;
  lua_pushnil(state);
  while (lua_next(state, 1) != 0) {
    if ((lua_type(state, -2) != LUA_TSTRING) || (keyCount >= kMaxSaveKeys)) {
      valid = false;
      lua_pop(state, 2);
      break;
    }
    const char *key = lua_tostring(state, -2);
    const int valueType = lua_type(state, -1);
    writer.begin_object();
    writer.write_string("k", key);
    if (valueType == LUA_TNUMBER) {
      writer.write_float("v", static_cast<float>(lua_tonumber(state, -1)));
    } else if (valueType == LUA_TSTRING) {
      writer.write_string("v", lua_tostring(state, -1));
    } else if (valueType == LUA_TBOOLEAN) {
      writer.write_bool("v", lua_toboolean(state, -1) != 0);
    } else {
      valid = false;
      writer.end_object();
      lua_pop(state, 2);
      break;
    }
    writer.end_object();
    ++keyCount;
    lua_pop(state, 1);
  }
  writer.end_array();
  writer.end_object();

  bool ok = false;
  if (valid && !writer.failed() &&
      (writer.result_size() <= kMaxSaveJsonBytes)) {
    ok = runtime_binding().services->save_game_data(writer.result(),
                                                    writer.result_size());
  }
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_load_data(lua_State *state) noexcept {
  if ((runtime_binding().services == nullptr) ||
      (runtime_binding().services->load_game_data == nullptr)) {
    lua_pushnil(state);
    return 1;
  }

  static char buffer[kMaxSaveJsonBytes + 1U];
  std::size_t length = 0U;
  if (!runtime_binding().services->load_game_data(buffer, sizeof(buffer),
                                                  &length)) {
    lua_pushnil(state);
    return 1;
  }

  core::JsonParser parser{};
  const core::JsonValue *root = nullptr;
  if (!parser.parse(buffer, length) || ((root = parser.root()) == nullptr) ||
      (root->type != core::JsonValue::Type::Object)) {
    lua_pushnil(state);
    return 1;
  }

  core::JsonValue entries{};
  if (!parser.get_object_field(*root, "entries", &entries)) {
    lua_pushnil(state);
    return 1;
  }

  lua_newtable(state);
  const std::size_t entryCount = parser.array_size(entries);
  for (std::size_t i = 0U; i < entryCount; ++i) {
    core::JsonValue entry{};
    core::JsonValue keyValue{};
    core::JsonValue value{};
    char key[128] = {};
    if (!parser.get_array_element(entries, i, &entry) ||
        !parser.get_object_field(entry, "k", &keyValue) ||
        !parser.copy_string(keyValue, key, sizeof(key)) ||
        !parser.get_object_field(entry, "v", &value)) {
      continue;
    }
    float number = 0.0F;
    bool flag = false;
    char text[256] = {};
    if (value.type == core::JsonValue::Type::Number &&
        parser.as_float(value, &number)) {
      lua_pushnumber(state, static_cast<lua_Number>(number));
    } else if (value.type == core::JsonValue::Type::Bool &&
               parser.as_bool(value, &flag)) {
      lua_pushboolean(state, flag ? 1 : 0);
    } else if (parser.copy_string(value, text, sizeof(text))) {
      lua_pushstring(state, text);
    } else {
      continue;
    }
    lua_setfield(state, -2, key);
  }
  return 1;
}

void clear_persist_bindings(lua_State *state) noexcept {
  if ((state != nullptr) && (g_persistRef != LUA_NOREF)) {
    luaL_unref(state, LUA_REGISTRYINDEX, g_persistRef);
  }
  g_persistRef = LUA_NOREF;
}

} // namespace engine::scripting
