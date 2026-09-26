// Owns Lua persistence bindings for the Engine scripting system: the
// in-memory hot-reload persist table plus the on-disk single-slot save
// (engine.save_data / engine.load_data, flat table <-> JSON, bounded by the
// save slot's document ceiling rather than by per-key or per-value caps).

#include "persist_bindings.h"

#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/scripting/runtime_services.h"

namespace engine::scripting {
namespace {

int g_persistRef = LUA_NOREF;

// A key names a value, so it is identity-bearing and has a fixed width:
// the loader copies it into a buffer of exactly this size and refuses one
// that does not fit, so the writer refuses the same keys up front instead
// of producing a file the loader will reject. Includes the terminator.
// Key count and string values are bounded only by the document ceiling.
constexpr std::size_t kMaxSaveKeyBytes = 128U;

/// Logs why engine.save_data refused the table; the script sees false.
void log_save_refusal(const char *key, const char *reason) noexcept {
  char message[256] = {};
  std::snprintf(message, sizeof(message),
                "engine.save_data refused: %s%s%s; nothing was written",
                reason, (key != nullptr) ? " at key " : "",
                (key != nullptr) ? key : "");
  core::log_message(core::LogLevel::Error, "scripting", message);
}

/// Logs why engine.load_data refused the document and returns nil to the
/// script; a malformed field refuses the load rather than substituting.
int refuse_load(lua_State *state, std::size_t entryIndex,
                const char *reason) noexcept {
  char message[192] = {};
  std::snprintf(message, sizeof(message),
                "engine.load_data refused the save: entry %zu %s", entryIndex,
                reason);
  core::log_message(core::LogLevel::Error, "scripting", message);
  lua_pop(state, 1);
  lua_pushnil(state);
  return 1;
}

/// Logs a save refused for exceeding the document ceiling.
void log_oversized_save() noexcept {
  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "engine.save_data refused: the document is larger than the "
                "%zu-byte save ceiling; nothing was written",
                kMaxGameSaveBytes);
  core::log_message(core::LogLevel::Error, "scripting", message);
}

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
  bool valid = true;
  lua_pushnil(state);
  while (lua_next(state, 1) != 0) {
    if (lua_type(state, -2) != LUA_TSTRING) {
      log_save_refusal(nullptr, "a key is not a string");
      valid = false;
      lua_pop(state, 2);
      break;
    }
    std::size_t keyLength = 0U;
    const char *key = lua_tolstring(state, -2, &keyLength);
    if ((keyLength >= kMaxSaveKeyBytes) || (std::strlen(key) != keyLength)) {
      log_save_refusal(nullptr, "a key is longer than 127 bytes or holds an "
                                "embedded NUL");
      valid = false;
      lua_pop(state, 2);
      break;
    }
    const int valueType = lua_type(state, -1);
    const char *reason = nullptr;
    if (valueType == LUA_TNUMBER) {
      if (lua_isinteger(state, -1) != 0) {
        writer.begin_object();
        writer.write_string("k", key);
        writer.write_int64("v",
                           static_cast<std::int64_t>(lua_tointeger(state, -1)));
        writer.end_object();
      } else {
        const double number = static_cast<double>(lua_tonumber(state, -1));
        if (!std::isfinite(number)) {
          reason = "the number is not finite";
        } else {
          writer.begin_object();
          writer.write_string("k", key);
          writer.write_double("v", number);
          writer.end_object();
        }
      }
    } else if (valueType == LUA_TSTRING) {
      std::size_t textLength = 0U;
      const char *text = lua_tolstring(state, -1, &textLength);
      if (std::strlen(text) != textLength) {
        reason = "the string holds an embedded NUL";
      } else {
        writer.begin_object();
        writer.write_string("k", key);
        writer.write_string("v", text);
        writer.end_object();
      }
    } else if (valueType == LUA_TBOOLEAN) {
      writer.begin_object();
      writer.write_string("k", key);
      writer.write_bool("v", lua_toboolean(state, -1) != 0);
      writer.end_object();
    } else {
      reason = "the value is not a number, string or boolean";
    }
    if (reason != nullptr) {
      log_save_refusal(key, reason);
      valid = false;
      lua_pop(state, 2);
      break;
    }
    // Checked per entry so a table far past the ceiling stops here
    // rather than growing the writer to hold all of it.
    if (writer.failed() || (writer.result_size() > kMaxGameSaveBytes)) {
      log_oversized_save();
      valid = false;
      lua_pop(state, 2);
      break;
    }
    lua_pop(state, 1);
  }
  writer.end_array();
  writer.end_object();

  bool ok = false;
  if (valid &&
      (writer.failed() || (writer.result_size() > kMaxGameSaveBytes))) {
    log_oversized_save();
    valid = false;
  }
  if (valid) {
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

  // A cold path: one ceiling-sized buffer from the Lua heap per load, kept
  // alive by its stack slot while the parser points into it and collected
  // after the call. Only the top value is returned, so the slot needs no
  // cleanup on any exit.
  auto *buffer =
      static_cast<char *>(lua_newuserdatauv(state, kMaxGameSaveBytes + 1U, 0));
  std::size_t length = 0U;
  if (!runtime_binding().services->load_game_data(
          buffer, kMaxGameSaveBytes + 1U, &length)) {
    lua_pushnil(state);
    return 1;
  }
  // Every decoded string fits in the document it came from, so one
  // scratch buffer of the document's size holds any value.
  auto *text = static_cast<char *>(lua_newuserdatauv(state, length + 1U, 0));

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
    char key[kMaxSaveKeyBytes] = {};
    if (!parser.get_array_element(entries, i, &entry) ||
        !parser.get_object_field(entry, "k", &keyValue) ||
        !parser.get_object_field(entry, "v", &value)) {
      return refuse_load(state, i, "is not a {k, v} object");
    }
    // Strict copies: a key or string the buffer cannot hold is a corrupt
    // or hand-edited save and refuses the load, never a truncated value
    // handed back under the cut spelling.
    if (!parser.copy_string_strict(keyValue, key, sizeof(key))) {
      return refuse_load(state, i, "has a key that is not a string of at "
                                   "most 127 bytes");
    }
    std::int64_t integer = 0;
    double number = 0.0;
    bool flag = false;
    if (value.type == core::JsonValue::Type::Number) {
      // Integer literals read back as Lua integers, everything else as
      // the double the writer produced at round-trip precision.
      if (parser.as_int64(value, &integer)) {
        lua_pushinteger(state, static_cast<lua_Integer>(integer));
      } else if (parser.as_double(value, &number)) {
        lua_pushnumber(state, static_cast<lua_Number>(number));
      } else {
        return refuse_load(state, i, "has a number that does not parse");
      }
    } else if (value.type == core::JsonValue::Type::Bool) {
      if (!parser.as_bool(value, &flag)) {
        return refuse_load(state, i, "has a boolean that does not parse");
      }
      lua_pushboolean(state, flag ? 1 : 0);
    } else if (value.type == core::JsonValue::Type::String) {
      std::size_t textLength = 0U;
      if (!parser.copy_string(value, text, length + 1U, &textLength) ||
          (textLength > length) || (std::strlen(text) != textLength)) {
        return refuse_load(state, i,
                           "has a string that does not decode or "
                           "holds a NUL byte");
      }
      lua_pushlstring(state, text, textLength);
    } else {
      return refuse_load(state, i, "has a value that is not a number, "
                                   "string or boolean");
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
