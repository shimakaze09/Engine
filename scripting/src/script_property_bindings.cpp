// Implements engine.get_property: a script reads one of its declared
// properties for an entity, getting the entity's own value when it has one
// and the script's default otherwise. The default and the declared type
// come from the module's `properties` table as Lua evaluated it, the same
// literal the editor's static scan reads.

#include "script_property_bindings.h"

#include <cstdio>
#include <cstring>

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "entity_handle.h"
#include "entity_script_bindings.h"
#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace engine::scripting {
namespace {

using math::ScriptPropertyType;

constexpr const char *kLogChannel = "script";

/// Problems already reported, by a hash of script path, property name and
/// kind, so a read in on_tick says what is wrong once, not every frame.
constexpr std::size_t kMaxReportedProblems = 64U;
std::uint64_t g_reported[kMaxReportedProblems] = {};
std::size_t g_reportedCount = 0U;

/// Folds `text` into a running FNV-1a hash.
std::uint64_t hash_append(std::uint64_t hash, const char *text) noexcept {
  for (const char *c = text; *c != '\0'; ++c) {
    hash = core::fnv1a_64_append(hash, static_cast<std::uint8_t>(*c));
  }
  return hash;
}

/// Logs a Warning once per script, property and kind of problem.
void report_once(const char *path, const char *name, const char *kind,
                 const char *message) noexcept {
  // The separator keeps "ab"+"c" and "a"+"bc" apart.
  std::uint64_t key = core::fnv1a_64(path);
  key = hash_append(core::fnv1a_64_append(key, 0U), name);
  key = hash_append(core::fnv1a_64_append(key, 0U), kind);
  for (std::size_t i = 0U; i < g_reportedCount; ++i) {
    if (g_reported[i] == key) {
      return;
    }
  }
  if (g_reportedCount < kMaxReportedProblems) {
    g_reported[g_reportedCount++] = key;
  }
  char text[512] = {};
  std::snprintf(text, sizeof(text), "%s: property '%s' %s", path, name,
                message);
  core::log_message(core::LogLevel::Warning, kLogChannel, text);
}

/// The type a declared default value has in Lua, or false when it is not
/// one a property holds.
bool lua_value_type(lua_State *state, int index,
                    ScriptPropertyType *out) noexcept {
  switch (lua_type(state, index)) {
  case LUA_TBOOLEAN:
    *out = ScriptPropertyType::Bool;
    return true;
  case LUA_TNUMBER:
    *out = (lua_isinteger(state, index) != 0) ? ScriptPropertyType::Integer
                                              : ScriptPropertyType::Float;
    return true;
  case LUA_TSTRING:
    *out = ScriptPropertyType::String;
    return true;
  default:
    return false;
  }
}

bool type_from_name(const char *name, ScriptPropertyType *out) noexcept {
  static constexpr struct {
    const char *name;
    ScriptPropertyType type;
  } kTypes[] = {{"bool", ScriptPropertyType::Bool},
                {"integer", ScriptPropertyType::Integer},
                {"float", ScriptPropertyType::Float},
                {"string", ScriptPropertyType::String}};
  for (const auto &entry : kTypes) {
    if (std::strcmp(name, entry.name) == 0) {
      *out = entry.type;
      return true;
    }
  }
  return false;
}

/// Pushes the zero value of `type`.
void push_zero(lua_State *state, ScriptPropertyType type) noexcept {
  switch (type) {
  case ScriptPropertyType::Bool:
    lua_pushboolean(state, 0);
    return;
  case ScriptPropertyType::Integer:
    lua_pushinteger(state, 0);
    return;
  case ScriptPropertyType::Float:
    lua_pushnumber(state, 0.0);
    return;
  case ScriptPropertyType::String:
    lua_pushliteral(state, "");
    return;
  }
  lua_pushnil(state);
}

/// Pushes the declaration of `name` in the module table at `moduleIndex`
/// as its default value and reads its type. False, with nothing pushed,
/// when the module declares no such property or declares it in a form no
/// property takes.
bool push_declared_default(lua_State *state, int moduleIndex, const char *name,
                           ScriptPropertyType *outType) noexcept {
  if (lua_getfield(state, moduleIndex, "properties") != LUA_TTABLE) {
    lua_pop(state, 1);
    return false;
  }
  const int declarations = lua_gettop(state);
  const int kind = lua_getfield(state, declarations, name);
  if (kind == LUA_TTABLE) {
    // The table form: { type = ..., default = ... }.
    const int entry = lua_gettop(state);
    bool typed = false;
    if (lua_getfield(state, entry, "type") == LUA_TSTRING) {
      typed = type_from_name(lua_tostring(state, -1), outType);
      if (!typed) {
        lua_pop(state, 3);
        return false;
      }
    }
    lua_pop(state, 1);
    if (lua_getfield(state, entry, "default") == LUA_TNIL) {
      lua_pop(state, 1);
      if (!typed) {
        lua_pop(state, 2);
        return false;
      }
      push_zero(state, *outType);
    } else {
      ScriptPropertyType valueType{};
      if (!lua_value_type(state, -1, &valueType)) {
        lua_pop(state, 3);
        return false;
      }
      if (!typed) {
        *outType = valueType;
      } else if ((*outType == ScriptPropertyType::Float) &&
                 (valueType == ScriptPropertyType::Integer)) {
        // An integer default of a float property reads as a float.
        const lua_Number value =
            static_cast<lua_Number>(lua_tointeger(state, -1));
        lua_pop(state, 1);
        lua_pushnumber(state, value);
      } else if (valueType != *outType) {
        lua_pop(state, 3);
        return false;
      }
    }
    // Leave only the default: drop the entry and the declarations table.
    lua_replace(state, declarations);
    lua_settop(state, declarations);
    return true;
  }
  if (!lua_value_type(state, -1, outType)) {
    lua_pop(state, 2);
    return false;
  }
  lua_replace(state, declarations);
  return true;
}

/// Pushes an override's value as the declared type reads it.
void push_override(lua_State *state,
                   const math::ScriptPropertyValue &value) noexcept {
  switch (value.type) {
  case ScriptPropertyType::Bool:
    lua_pushboolean(state, value.boolValue ? 1 : 0);
    return;
  case ScriptPropertyType::Integer:
    lua_pushinteger(state, static_cast<lua_Integer>(value.integerValue));
    return;
  case ScriptPropertyType::Float:
    lua_pushnumber(state, static_cast<lua_Number>(value.floatValue));
    return;
  case ScriptPropertyType::String:
    lua_pushstring(state, value.text);
    return;
  }
  lua_pushnil(state);
}

} // namespace

// engine.get_property(entity, name) -> the entity's value of the property,
// or the declaring script's default when the entity has none; nil, with a
// Warning once per script and property, when no behaviour of the entity
// declares it or the entity has no script. Inside a behaviour's own hook,
// `entity` being that hook's entity, the property is that behaviour's;
// otherwise it is the first of the entity's behaviours that declares it.
int lua_engine_get_property(lua_State *state) noexcept {
  runtime::Entity entity{};
  const char *name = lua_tostring(state, 2);
  if (!read_entity(state, 1, &entity) || (name == nullptr) ||
      !runtime_bound()) {
    lua_pushnil(state);
    return 1;
  }
  const RuntimeServices &services = *runtime_binding().services;
  runtime::World *world = runtime_binding().world;
  // Copied, since loading a module runs Lua that may edit the list.
  runtime::ScriptComponent script{};
  const runtime::ScriptComponent *live =
      (services.find_script_component_op != nullptr)
          ? services.find_script_component_op(world, entity)
          : nullptr;
  if (live != nullptr) {
    script = *live;
  }
  const std::size_t count = math::script_behaviour_count(script);
  if (count == 0U) {
    report_once("(no script)", name, "noscript",
                "was read for an entity with no script");
    lua_pushnil(state);
    return 1;
  }
  std::size_t first = 0U;
  std::size_t last = count;
  const std::size_t running =
      math::find_script_behaviour(script, running_hook_script(entity));
  if (running < count) {
    first = running;
    last = running + 1U;
  }
  const int top = lua_gettop(state);
  std::size_t behaviour = count;
  ScriptPropertyType declaredType{};
  for (std::size_t b = first; (b < last) && (behaviour == count); ++b) {
    lua_settop(state, top);
    if (push_entity_script_module(state, script.behaviours[b].scriptPath) &&
        push_declared_default(state, lua_gettop(state), name, &declaredType)) {
      behaviour = b;
    }
  }
  if (behaviour == count) {
    report_once(script.behaviours[first].scriptPath, name, "undeclared",
                (last - first == 1U)
                    ? "is not declared in the script's properties table, or "
                      "not as a bool, integer, float or string"
                    : "is not declared in any of the entity's scripts, or "
                      "not as a bool, integer, float or string");
    lua_settop(state, top);
    lua_pushnil(state);
    return 1;
  }
  const char *scriptPath = script.behaviours[behaviour].scriptPath;
  // Stack: module, default.
  const math::ScriptPropertiesComponent *overrides =
      (services.find_script_properties_op != nullptr)
          ? services.find_script_properties_op(world, entity)
          : nullptr;
  const math::ScriptPropertiesComponent::Override *entry =
      (overrides != nullptr)
          ? math::script_property_override(*overrides, behaviour, name)
          : nullptr;
  if ((entry != nullptr) && (entry->value.type != declaredType)) {
    // The script changed the property's type since the value was set.
    report_once(scriptPath, name, "mismatch",
                "has a stored value of another type than the script now "
                "declares; the default is used");
    entry = nullptr;
  }
  if (entry != nullptr) {
    push_override(state, entry->value);
  } else {
    lua_pushvalue(state, -1);
  }
  lua_replace(state, top + 1);
  lua_settop(state, top + 1);
  return 1;
}

void reset_script_property_reports() noexcept { g_reportedCount = 0U; }

} // namespace engine::scripting
