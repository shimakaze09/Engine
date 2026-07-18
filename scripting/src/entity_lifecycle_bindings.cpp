// Implements entity lifecycle Lua bindings (spawn/destroy/liveness, names,
// clones)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "entity_lifecycle_bindings.h"

#include "binding_util.h"
#include "deferred_mutations.h"
#include "entity_handle.h"
#include "game_bindings.h"
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

void copy_clone_name(char *destination, std::size_t destinationSize,
                     const char *source) noexcept {
  constexpr const char *kCloneSuffix = " (clone)";
  constexpr std::size_t kCloneSuffixLength = 8U;

  if ((destination == nullptr) || (destinationSize == 0U)) {
    return;
  }

  destination[0] = '\0';
  if (source == nullptr) {
    return;
  }

  const std::size_t maxPrefixLength =
      (destinationSize > (kCloneSuffixLength + 1U))
          ? (destinationSize - kCloneSuffixLength - 1U)
          : 0U;
  const std::size_t sourceLength = std::strlen(source);
  const std::size_t prefixLength =
      (sourceLength < maxPrefixLength) ? sourceLength : maxPrefixLength;
  if (prefixLength > 0U) {
    std::memcpy(destination, source, prefixLength);
  }
  if ((prefixLength + kCloneSuffixLength) < destinationSize) {
    std::memcpy(destination + prefixLength, kCloneSuffix, kCloneSuffixLength);
    destination[prefixLength + kCloneSuffixLength] = '\0';
  }
}

int lua_engine_log(lua_State *state) noexcept {
  const char *message = lua_tostring(state, 1);
  if (message == nullptr) {
    message = "";
  }

  core::log_message(core::LogLevel::Info, "script", message);
  return 0;
}

int lua_engine_get_entity_count(lua_State *state) noexcept {
  const std::size_t count = (runtime_binding().world != nullptr && runtime_binding().services != nullptr)
                                ? runtime_binding().services->get_transform_count(runtime_binding().world)
                                : 0U;
  lua_pushinteger(state, static_cast<lua_Integer>(count));
  return 1;
}

int lua_engine_spawn_entity(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      !can_apply_mutations_now()) {
    lua_pushnil(state);
    return 1;
  }

  // Only valid during Input; create_entity returns 0 otherwise.
  const std::uint32_t entityIndex = runtime_binding().services->create_entity_op(runtime_binding().world);
  if (entityIndex == 0U) {
    lua_pushnil(state);
    return 1;
  }

  push_entity_handle_from_index(state, entityIndex);
  return 1;
}

int lua_engine_destroy_entity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  clear_player_controller_entity(entity);
  const bool ok = apply_or_queue_destroy_entity(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_is_alive(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Entity entity{};
  if (!decode_lua_entity_handle(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state, runtime_binding().world->is_alive(entity) ? 1 : 0);
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
  if ((runtime_binding().services == nullptr) ||
      !runtime_binding().services->get_name_component_op(runtime_binding().world, entity.index, &component)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushstring(state, component.name);
  return 1;
}

int lua_engine_find_by_name(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr || !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  const char *searchName = lua_tostring(state, 1);
  if (searchName == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity found = runtime_binding().world->find_entity_by_name(searchName);

  if (found == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle(state, found);
  return 1;
}

int lua_engine_clone_entity(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !can_apply_mutations_now()) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity source{};
  if (!read_entity(state, 1, &source)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity newEntity = runtime_binding().world->create_entity();
  if (newEntity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  // Copy Transform.
  runtime::Transform transform{};
  if (runtime_binding().world->get_transform(source, &transform)) {
    static_cast<void>(runtime_binding().world->add_transform(newEntity, transform));
  }

  // Copy RigidBody.
  runtime::RigidBody rigidBody{};
  if (runtime_binding().world->get_rigid_body(source, &rigidBody)) {
    static_cast<void>(runtime_binding().world->add_rigid_body(newEntity, rigidBody));
  }

  // Copy Collider.
  runtime::Collider collider{};
  if (runtime_binding().world->get_collider(source, &collider)) {
    static_cast<void>(runtime_binding().world->add_collider(newEntity, collider));
  }

  // Copy MeshComponent.
  runtime::MeshComponent mesh{};
  if (runtime_binding().world->get_mesh_component(source, &mesh)) {
    static_cast<void>(runtime_binding().world->add_mesh_component(newEntity, mesh));
  }

  // Copy NameComponent with "(clone)" suffix.
  runtime::NameComponent name{};
  if (runtime_binding().world->get_name_component(source, &name)) {
    runtime::NameComponent cloneName{};
    copy_clone_name(cloneName.name, sizeof(cloneName.name), name.name);
    static_cast<void>(runtime_binding().world->add_name_component(newEntity, cloneName));
  }

  // Copy LightComponent.
  runtime::LightComponent light{};
  if (runtime_binding().world->get_light_component(source, &light)) {
    static_cast<void>(runtime_binding().world->add_light_component(newEntity, light));
  }

  push_entity_handle(state, newEntity);
  return 1;
}

// --- Prefab bindings ---

} // namespace

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_entity_lifecycle_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_log);
  lua_setfield(state, -2, "log");
  lua_pushcfunction(state, &lua_engine_get_entity_count);
  lua_setfield(state, -2, "get_entity_count");
  lua_pushcfunction(state, &lua_engine_spawn_entity);
  lua_setfield(state, -2, "spawn_entity");
  lua_pushcfunction(state, &lua_engine_destroy_entity);
  lua_setfield(state, -2, "destroy_entity");
  lua_pushcfunction(state, &lua_engine_is_alive);
  lua_setfield(state, -2, "is_alive");
  lua_pushcfunction(state, &lua_engine_set_name);
  lua_setfield(state, -2, "set_name");
  lua_pushcfunction(state, &lua_engine_get_name);
  lua_setfield(state, -2, "get_name");
  lua_pushcfunction(state, &lua_engine_find_by_name);
  lua_setfield(state, -2, "find_entity_by_name");
  lua_pushcfunction(state, &lua_engine_clone_entity);
  lua_setfield(state, -2, "clone_entity");
}

} // namespace engine::scripting
