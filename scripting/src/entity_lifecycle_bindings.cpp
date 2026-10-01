// Implements entity lifecycle Lua bindings (spawn/destroy/liveness, names,
// clones)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "entity_lifecycle_bindings.h"

#include "binding_util.h"
#include "deferred_mutations.h"
#include "entity_handle.h"
#include "lua_state.h"
#include "reload_transaction.h"
#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/quat.h"
#include "engine/scripting/runtime_services.h"

namespace engine::scripting {

namespace {

int lua_engine_log(lua_State *state) noexcept {
  const char *message = lua_tostring(state, 1);
  if (message == nullptr) {
    message = "";
  }

  core::log_message(core::LogLevel::Info, "scripting", message);
  return 0;
}

int lua_engine_spawn_entity(lua_State *state) noexcept {
  if (!can_create_entities_now() ||
      (reload_staging(ReloadEffect::CreateEntity) == ReloadStaging::Refused)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity entity =
      runtime_binding().services->create_scene_object_op(
          runtime_binding().world, nullptr);
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  reload_note_created_entity(entity);
  push_entity_handle(state, entity);
  return 1;
}

int lua_engine_destroy_entity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state, apply_or_queue_destroy_entity(entity) ? 1 : 0);
  return 1;
}


int lua_engine_set_name(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isstring(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *name = lua_tostring(state, 2);
  if (name == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::NameComponent component{};
  const std::size_t nameLength = std::strlen(name);
  constexpr std::size_t kMaxNameLength = sizeof(component.name) - 1U;
  if (nameLength > kMaxNameLength) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_name truncated input to NameComponent capacity");
  }
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  component.name[sizeof(component.name) - 1U] = '\0';

  const bool ok = apply_or_queue_name_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_name(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::NameComponent component{};
  if (!latest_name_component(entity, &component)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushstring(state, component.name);
  return 1;
}

int lua_engine_find_by_name(lua_State *state) noexcept {
  if (!runtime_bound() || !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  const char *searchName = lua_tostring(state, 1);
  if (searchName == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity found =
      runtime_binding().services->find_entity_by_name(runtime_binding().world,
                                                      searchName);

  if (found == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle(state, found);
  return 1;
}

// Most entities engine.find_entities_by_tag returns in one call; its second
// result says how many carry the tag, so a capped list is never mistaken
// for the whole.
constexpr std::size_t kMaxTagQueryResults = 1024U;

/// Reads a tag argument; nullptr when it is not a string.
const char *read_tag_arg(lua_State *state, int index) noexcept {
  return (lua_type(state, index) == LUA_TSTRING) ? lua_tostring(state, index)
                                                 : nullptr;
}

// engine.add_tag(entity, tag) -> true, or false and a reason. A tag the
// entity already carries (ignoring case) is success.
int lua_engine_add_tag(lua_State *state) noexcept {
  runtime::Entity entity{};
  const char *tag = read_tag_arg(state, 2);
  if (!read_entity(state, 1, &entity) || (tag == nullptr)) {
    lua_pushboolean(state, 0);
    lua_pushliteral(state, "add_tag expects an entity and a tag string");
    return 2;
  }
  math::TagSetComponent tags{};
  static_cast<void>(latest_tag_set_component(entity, &tags));
  switch (math::tag_set_add(&tags, tag)) {
  case math::TagSetAdd::AlreadyPresent:
    lua_pushboolean(state, 1);
    return 1;
  case math::TagSetAdd::InvalidTag:
    lua_pushboolean(state, 0);
    lua_pushliteral(state, "a tag is 1 to 31 letters, digits, '_', '-' or '.'");
    return 2;
  case math::TagSetAdd::Full:
    lua_pushboolean(state, 0);
    lua_pushliteral(state, "an entity carries at most 8 tags");
    return 2;
  case math::TagSetAdd::Added:
    break;
  }
  if (!apply_or_queue_tag_set_component(entity, tags)) {
    lua_pushboolean(state, 0);
    lua_pushliteral(state, "the entity cannot take tags (the Log says why)");
    return 2;
  }
  lua_pushboolean(state, 1);
  return 1;
}

// engine.remove_tag(entity, tag) -> true when the entity carried it. The
// last tag removed takes the Tags component with it.
int lua_engine_remove_tag(lua_State *state) noexcept {
  runtime::Entity entity{};
  const char *tag = read_tag_arg(state, 2);
  math::TagSetComponent tags{};
  if (!read_entity(state, 1, &entity) || (tag == nullptr) ||
      !latest_tag_set_component(entity, &tags) ||
      !math::tag_set_remove(&tags, tag)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = (tags.count == 0U)
                      ? apply_or_queue_remove_tag_set_component(entity)
                      : apply_or_queue_tag_set_component(entity, tags);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.has_tag(entity, tag) -> bool, ignoring case.
int lua_engine_has_tag(lua_State *state) noexcept {
  runtime::Entity entity{};
  const char *tag = read_tag_arg(state, 2);
  math::TagSetComponent tags{};
  const bool has = read_entity(state, 1, &entity) && (tag != nullptr) &&
                   latest_tag_set_component(entity, &tags) &&
                   math::tag_set_has(tags, tag);
  lua_pushboolean(state, has ? 1 : 0);
  return 1;
}

// engine.get_tags(entity) -> array of the entity's tags in order (empty
// when it has none), or nil for an entity that does not exist.
int lua_engine_get_tags(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  math::TagSetComponent tags{};
  static_cast<void>(latest_tag_set_component(entity, &tags));
  lua_createtable(state, static_cast<int>(tags.count), 0);
  for (std::uint32_t i = 0U; i < tags.count; ++i) {
    lua_pushstring(state, tags.tags[i]);
    lua_rawseti(state, -2, static_cast<lua_Integer>(i) + 1);
  }
  return 1;
}

// engine.find_entities_by_tag(tag) -> array of entities carrying the tag
// (ignoring case) in ascending entity index, then how many carry it; the
// array holds at most kMaxTagQueryResults.
int lua_engine_find_entities_by_tag(lua_State *state) noexcept {
  const char *tag = read_tag_arg(state, 1);
  if (!runtime_bound() ||
      (runtime_binding().services->find_entities_by_tag == nullptr) ||
      (tag == nullptr)) {
    lua_createtable(state, 0, 0);
    lua_pushinteger(state, 0);
    return 2;
  }
  static runtime::Entity found[kMaxTagQueryResults] = {};
  const std::size_t total = runtime_binding().services->find_entities_by_tag(
      runtime_binding().world, tag, found, kMaxTagQueryResults);
  const std::size_t kept =
      (total < kMaxTagQueryResults) ? total : kMaxTagQueryResults;
  lua_createtable(state, static_cast<int>(kept), 0);
  for (std::size_t i = 0U; i < kept; ++i) {
    push_entity_handle(state, found[i]);
    lua_rawseti(state, -2, static_cast<lua_Integer>(i) + 1);
  }
  lua_pushinteger(state, static_cast<lua_Integer>(total));
  return 2;
}

int lua_engine_clone_entity(lua_State *state) noexcept {
  if (!runtime_bound() ||
      (runtime_binding().services->clone_entity_op == nullptr) ||
      !can_create_entities_now()) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity source{};
  if (!read_entity(state, 1, &source) ||
      (reload_staging(ReloadEffect::CreateEntity) == ReloadStaging::Refused)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity clone = runtime_binding().services->clone_entity_op(
      runtime_binding().world, source);
  if (clone == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  reload_note_created_entity(clone);
  push_entity_handle(state, clone);
  return 1;
}

// --- Prefab bindings ---

} // namespace

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_entity_lifecycle_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_log);
  lua_setfield(state, -2, "log");
  lua_pushcfunction(state, &lua_engine_spawn_entity);
  lua_setfield(state, -2, "spawn_entity");
  lua_pushcfunction(state, &lua_engine_destroy_entity);
  lua_setfield(state, -2, "destroy_entity");
  lua_pushcfunction(state, &lua_engine_set_name);
  lua_setfield(state, -2, "set_name");
  lua_pushcfunction(state, &lua_engine_get_name);
  lua_setfield(state, -2, "get_name");
  lua_pushcfunction(state, &lua_engine_find_by_name);
  lua_setfield(state, -2, "find_entity_by_name");
  lua_pushcfunction(state, &lua_engine_clone_entity);
  lua_setfield(state, -2, "clone_entity");
  lua_pushcfunction(state, &lua_engine_add_tag);
  lua_setfield(state, -2, "add_tag");
  lua_pushcfunction(state, &lua_engine_remove_tag);
  lua_setfield(state, -2, "remove_tag");
  lua_pushcfunction(state, &lua_engine_has_tag);
  lua_setfield(state, -2, "has_tag");
  lua_pushcfunction(state, &lua_engine_get_tags);
  lua_setfield(state, -2, "get_tags");
  lua_pushcfunction(state, &lua_engine_find_entities_by_tag);
  lua_setfield(state, -2, "find_entities_by_tag");
}

} // namespace engine::scripting
