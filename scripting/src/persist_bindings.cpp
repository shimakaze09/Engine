// Owns Lua persistence bindings for the Engine scripting system: the
// in-memory hot-reload persist table plus the on-disk save slots
// (engine.save_data / engine.load_data / engine.discard_save /
// engine.list_saves, a versioned flat table <-> JSON per named slot,
// bounded by the project's save limit rather than by per-key or per-value
// caps). A slot that does not load is held, never overwritten, until the
// game discards it.

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
#include <new>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
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

/// The save document's format version, written as "version" at its root.
/// A document naming a newer one came from a newer build and is held, not
/// read; one naming none predates the key and reads as this version.
constexpr std::int64_t kSaveFormatVersion = 1;

/// The slot a save binding names at stack index `index`: "default" when
/// the argument is absent or nil, otherwise a slot name. Anything else is
/// an argument error, so a typo never saves to or loads from another
/// slot.
const char *slot_argument(lua_State *state, int index) noexcept {
  if (lua_isnoneornil(state, index)) {
    return "default";
  }
  if (lua_type(state, index) != LUA_TSTRING) {
    luaL_argerror(state, index, "a save slot name is a string");
    return nullptr;
  }
  std::size_t length = 0U;
  const char *slot = lua_tolstring(state, index, &length);
  if ((std::strlen(slot) != length) ||
      !core::name_token_is_valid(slot, kGameSaveSlotNameCapacity - 1U)) {
    luaL_argerror(state, index,
                  "a save slot name is 1 to 31 bytes of letters, digits, '_', "
                  "'-' or '.'");
    return nullptr;
  }
  return slot;
}

/// Logs why engine.save_data refused the table; the script sees false.
void log_save_refusal(const char *key, const char *reason) noexcept {
  char message[256] = {};
  std::snprintf(message, sizeof(message),
                "engine.save_data refused: %s%s%s; nothing was written",
                reason, (key != nullptr) ? " at key " : "",
                (key != nullptr) ? key : "");
  core::log_message(core::LogLevel::Error, "scripting", message);
}

/// Pushes engine.load_data's two results: nil and `status`.
int push_load_failure(lua_State *state, const char *status) noexcept {
  lua_pushnil(state);
  lua_pushstring(state, status);
  return 2;
}

/// Logs why engine.load_data refused the slot, holds it so the next
/// save_data cannot replace what may be the only copy of the player's
/// progress, and returns (nil, `status`) to the script.
int refuse_document(lua_State *state, const char *slot, const char *status,
                    const char *reason) noexcept {
  char message[360] = {};
  std::snprintf(message, sizeof(message),
                "engine.load_data refused save slot '%s': %s; it is kept and "
                "engine.save_data refuses it until engine.discard_save "
                "moves it aside",
                slot, reason);
  core::log_message(core::LogLevel::Error, "scripting", message);
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->hold_game_save != nullptr)) {
    runtime_binding().services->hold_game_save(slot);
  }
  return push_load_failure(state, status);
}

/// refuse_document for one malformed entry; drops the half-built table.
int refuse_entry(lua_State *state, const char *slot, std::size_t entryIndex,
                 const char *reason) noexcept {
  char detail[192] = {};
  std::snprintf(detail, sizeof(detail), "entry %zu %s", entryIndex, reason);
  lua_pop(state, 1);
  return refuse_document(state, slot, "corrupt", detail);
}

/// Logs a save refused for exceeding the project's save limit.
void log_oversized_save(std::size_t limit) noexcept {
  char message[200] = {};
  std::snprintf(message, sizeof(message),
                "engine.save_data refused: the document is larger than the "
                "project's %zu-byte save limit; nothing was written",
                limit);
  core::log_message(core::LogLevel::Error, "scripting", message);
}

/// The status name engine.load_data and engine.list_saves report.
const char *read_status_name(GameSaveRead read) noexcept {
  switch (read) {
  case GameSaveRead::Ok:
    return "ok";
  case GameSaveRead::Absent:
    return "absent";
  case GameSaveRead::Corrupt:
    return "corrupt";
  case GameSaveRead::Unsupported:
    return "unsupported";
  case GameSaveRead::Unreadable:
    break;
  }
  return "unreadable";
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
  const char *slot = slot_argument(state, 2);
  if (!lua_istable(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const RuntimeServices *services = runtime_binding().services;
  if ((services == nullptr) || (services->save_game_data == nullptr) ||
      (services->game_save_limit == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const std::size_t limit = services->game_save_limit();

  core::JsonWriter writer{};
  writer.begin_object();
  writer.write_int64("version", kSaveFormatVersion);
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
    if (writer.failed() || (writer.result_size() > limit)) {
      log_oversized_save(limit);
      valid = false;
      lua_pop(state, 2);
      break;
    }
    lua_pop(state, 1);
  }
  writer.end_array();
  writer.end_object();

  bool ok = false;
  if (valid && (writer.failed() || (writer.result_size() > limit))) {
    log_oversized_save(limit);
    valid = false;
  }
  if (valid) {
    ok = services->save_game_data(slot, writer.result(), writer.result_size());
  }
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_load_data(lua_State *state) noexcept {
  const char *slot = slot_argument(state, 1);
  if ((runtime_binding().services == nullptr) ||
      (runtime_binding().services->load_game_data == nullptr)) {
    return push_load_failure(state, "unreadable");
  }

  // The runtime owns the payload until the next load, so nothing here
  // calls back into it while the parser points into the payload.
  const char *buffer = nullptr;
  std::size_t length = 0U;
  const GameSaveRead read =
      runtime_binding().services->load_game_data(slot, &buffer, &length);
  if (read == GameSaveRead::Absent) {
    return push_load_failure(state, "absent");
  }
  if (read != GameSaveRead::Ok) {
    const char *reason =
        (read == GameSaveRead::Corrupt)       ? "the file is damaged or cut off"
        : (read == GameSaveRead::Unsupported) ? "a newer build wrote it"
                                              : "the file could not be read";
    return refuse_document(state, slot, read_status_name(read), reason);
  }
  // A cold path: one scratch buffer from the Lua heap per load, kept alive
  // by its stack slot and collected after the call. Every decoded string
  // fits in the document it came from, so the document's size holds any
  // value. Only the top value is returned, so the slot needs no cleanup on
  // any exit.
  auto *text = static_cast<char *>(lua_newuserdatauv(state, length + 1U, 0));

  core::JsonParser parser{};
  if (!parser.parse(buffer, length)) {
    char reason[96] = {};
    std::snprintf(reason, sizeof(reason),
                  "it is not valid JSON (it breaks near byte %zu)",
                  parser.error_offset());
    return refuse_document(state, slot, "corrupt", reason);
  }
  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    return refuse_document(state, slot, "corrupt", "its root is not an object");
  }
  core::JsonValue versionValue{};
  if (parser.get_object_field(*root, "version", &versionValue)) {
    std::int64_t version = 0;
    if (!parser.as_int64(versionValue, &version) || (version < 1)) {
      return refuse_document(state, slot, "corrupt",
                             "its version is not a positive integer");
    }
    if (version > kSaveFormatVersion) {
      char reason[128] = {};
      std::snprintf(reason, sizeof(reason),
                    "a newer build wrote it (version %lld; this build reads "
                    "%lld)",
                    static_cast<long long>(version),
                    static_cast<long long>(kSaveFormatVersion));
      return refuse_document(state, slot, "unsupported", reason);
    }
  }

  core::JsonValue entries{};
  if (!parser.get_object_field(*root, "entries", &entries) ||
      (entries.type != core::JsonValue::Type::Array)) {
    return refuse_document(state, slot, "corrupt", "it has no entries array");
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
      return refuse_entry(state, slot, i, "is not a {k, v} object");
    }
    // Strict copies: a key or string the buffer cannot hold is a corrupt
    // or hand-edited save and refuses the load, never a truncated value
    // handed back under the cut spelling.
    if (!parser.copy_string_strict(keyValue, key, sizeof(key))) {
      return refuse_entry(state, slot, i,
                          "has a key that is not a string of at "
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
        return refuse_entry(state, slot, i, "has a number that does not parse");
      }
    } else if (value.type == core::JsonValue::Type::Bool) {
      if (!parser.as_bool(value, &flag)) {
        return refuse_entry(state, slot, i,
                            "has a boolean that does not parse");
      }
      lua_pushboolean(state, flag ? 1 : 0);
    } else if (value.type == core::JsonValue::Type::String) {
      std::size_t textLength = 0U;
      if (!parser.copy_string(value, text, length + 1U, &textLength) ||
          (textLength > length) || (std::strlen(text) != textLength)) {
        return refuse_entry(state, slot, i,
                            "has a string that does not decode or "
                            "holds a NUL byte");
      }
      lua_pushlstring(state, text, textLength);
    } else {
      return refuse_entry(state, slot, i,
                          "has a value that is not a number, "
                          "string or boolean");
    }
    lua_setfield(state, -2, key);
  }
  lua_pushstring(state, "ok");
  return 2;
}

int lua_engine_discard_save(lua_State *state) noexcept {
  const char *slot = slot_argument(state, 1);
  const bool discarded =
      (runtime_binding().services != nullptr) &&
      (runtime_binding().services->discard_game_save != nullptr) &&
      runtime_binding().services->discard_game_save(slot);
  lua_pushboolean(state, discarded ? 1 : 0);
  return 1;
}

int lua_engine_list_saves(lua_State *state) noexcept {
  const RuntimeServices *services = runtime_binding().services;
  lua_newtable(state);
  if ((services == nullptr) || (services->list_game_saves == nullptr)) {
    return 1;
  }
  // A cold path: the listing's scratch comes from the Lua heap and is
  // collected after the call.
  auto *slots = static_cast<GameSaveSlotInfo *>(lua_newuserdatauv(
      state, sizeof(GameSaveSlotInfo) * kMaxGameSaveSlots, 0));
  for (std::size_t i = 0U; i < kMaxGameSaveSlots; ++i) {
    new (&slots[i]) GameSaveSlotInfo{};
  }
  const std::size_t total = services->list_game_saves(slots, kMaxGameSaveSlots);
  const std::size_t count =
      (total < kMaxGameSaveSlots) ? total : kMaxGameSaveSlots;
  if (total > kMaxGameSaveSlots) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "engine.list_saves found %zu save slots and lists the "
                  "first %zu by name",
                  total, kMaxGameSaveSlots);
    core::log_message(core::LogLevel::Warning, "scripting", message);
  }
  for (std::size_t i = 0U; i < count; ++i) {
    const GameSaveSlotInfo &info = slots[i];
    lua_createtable(state, 0, 5);
    lua_pushstring(state, info.slot);
    lua_setfield(state, -2, "slot");
    lua_pushinteger(state, static_cast<lua_Integer>(info.savedAt));
    lua_setfield(state, -2, "saved_at");
    lua_pushinteger(state, static_cast<lua_Integer>(info.payloadBytes));
    lua_setfield(state, -2, "bytes");
    lua_pushstring(state, read_status_name(info.status));
    lua_setfield(state, -2, "status");
    lua_pushboolean(state, info.legacy ? 1 : 0);
    lua_setfield(state, -2, "legacy");
    lua_rawseti(state, -3, static_cast<lua_Integer>(i + 1U));
  }
  lua_pop(state, 1);
  return 1;
}

int lua_engine_get_save_limit(lua_State *state) noexcept {
  const RuntimeServices *services = runtime_binding().services;
  const std::size_t limit =
      ((services != nullptr) && (services->game_save_limit != nullptr))
          ? services->game_save_limit()
          : 0U;
  lua_pushinteger(state, static_cast<lua_Integer>(limit));
  return 1;
}

void clear_persist_bindings(lua_State *state) noexcept {
  if ((state != nullptr) && (g_persistRef != LUA_NOREF)) {
    luaL_unref(state, LUA_REGISTRYINDEX, g_persistRef);
  }
  g_persistRef = LUA_NOREF;
}

} // namespace engine::scripting
