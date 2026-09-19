// Implements private Lua game state and player binding helpers.

#include "game_bindings.h"

#include "entity_handle.h"

extern "C" {
#include "lua.h"
}

#include <cstddef>
#include <cstdio>

#include "engine/scripting/game_binding_state.h"
#include "runtime_binding.h"

namespace engine::scripting {
namespace {

constexpr std::size_t kMaxPlayerControllers =
    GameBindingState::kMaxPlayerControllers;

// Pipeline-owned when bound; the fallback keeps standalone/test
// use (no pipeline) working with identical semantics.
GameBindingState g_fallbackState{};
GameBindingState *g_boundState = nullptr;

/// Returns the state instance the game bindings currently act on.
GameBindingState &binding_state() noexcept {
  return (g_boundState != nullptr) ? *g_boundState : g_fallbackState;
}

/// Stores the current game mode name in scripting and the bound world.
bool set_game_mode_name(const char *name) noexcept {
  if (name == nullptr) {
    return false;
  }
  std::snprintf(binding_state().gameMode, sizeof(binding_state().gameMode), "%s", name);
  if (runtime_bound()) {
    static_cast<void>(runtime_binding().services->set_game_mode_name(
        runtime_binding().world, name));
  }
  return true;
}

/// Stores the current scripting game-state label.
bool set_game_state_name(const char *name) noexcept {
  if (name == nullptr) {
    return false;
  }
  std::snprintf(binding_state().gameState, sizeof(binding_state().gameState), "%s", name);
  return true;
}

} // namespace

int lua_engine_game_mode_start(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().services->game_mode_start(
                            runtime_binding().world)
                            ? 1
                            : 0);
  return 1;
}

int lua_engine_game_mode_pause(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().services->game_mode_pause(
                            runtime_binding().world)
                            ? 1
                            : 0);
  return 1;
}

int lua_engine_game_mode_end(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().services->game_mode_end(
                            runtime_binding().world)
                            ? 1
                            : 0);
  return 1;
}

int lua_engine_game_mode_state(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushstring(state, "none");
    return 1;
  }
  using S = GameModeState;
  switch (runtime_binding().services->game_mode_state(runtime_binding().world)) {
  case S::WaitingToStart:
    lua_pushstring(state, "waiting_to_start");
    break;
  case S::InProgress:
    lua_pushstring(state, "in_progress");
    break;
  case S::Paused:
    lua_pushstring(state, "paused");
    break;
  case S::Ended:
    lua_pushstring(state, "ended");
    break;
  }
  return 1;
}

int lua_engine_game_mode_set_rule(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *key = lua_tostring(state, 1);
  const char *value = lua_tostring(state, 2);
  if (key == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().services->game_mode_set_rule(
                             runtime_binding().world, key, value)
                             ? 1
                             : 0);
  return 1;
}

int lua_engine_game_mode_get_rule(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushnil(state);
    return 1;
  }
  const char *key = lua_tostring(state, 1);
  const char *value = runtime_binding().services->game_mode_get_rule(
      runtime_binding().world, key);
  if (value != nullptr) {
    lua_pushstring(state, value);
  } else {
    lua_pushnil(state);
  }
  return 1;
}

int lua_engine_game_mode_max_players(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushinteger(state, 0);
    return 1;
  }
  if ((lua_gettop(state) >= 1) && (lua_isnumber(state, 1) != 0)) {
    const auto n = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    runtime_binding().services->set_game_mode_max_players(
        runtime_binding().world, n);
  }
  lua_pushinteger(state, static_cast<lua_Integer>(
                             runtime_binding().services->game_mode_max_players(
                                 runtime_binding().world)));
  return 1;
}

int lua_engine_game_state_set_number(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  if ((key == nullptr) || (lua_isnumber(state, 2) == 0)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, binding_state().persistentState.set_number(
                             key, static_cast<float>(lua_tonumber(state, 2)))
                             ? 1
                             : 0);
  return 1;
}

int lua_engine_game_state_get_number(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  lua_pushnumber(
      state, static_cast<lua_Number>(binding_state().persistentState.get_number(key)));
  return 1;
}

int lua_engine_game_state_set_string(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  const char *value = lua_tostring(state, 2);
  if (key == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, binding_state().persistentState.set_string(key, value) ? 1 : 0);
  return 1;
}

int lua_engine_game_state_get_string(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  const char *value = binding_state().persistentState.get_string(key);
  if (value != nullptr) {
    lua_pushstring(state, value);
  } else {
    lua_pushnil(state);
  }
  return 1;
}

int lua_engine_game_state_has(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  lua_pushboolean(state, binding_state().persistentState.has(key) ? 1 : 0);
  return 1;
}

int lua_engine_game_state_clear(lua_State *state) noexcept {
  static_cast<void>(state);
  binding_state().persistentState.clear();
  return 0;
}

bool set_player_controller_entity(std::uint8_t player,
                                  core::Entity entity) noexcept {
  if (player >= kMaxPlayerControllers) {
    return false;
  }
  binding_state().playerControllerEntities[player] = entity;
  binding_state().playerControllers.set_controlled_entity(player, entity);
  return true;
}

core::Entity get_player_controller_entity(std::uint8_t player) noexcept {
  if (player >= kMaxPlayerControllers) {
    return core::kInvalidEntity;
  }
  return binding_state().playerControllers.get_controlled_entity(player);
}

bool is_player_controller_entity(core::Entity entity) noexcept {
  if (entity == core::kInvalidEntity) {
    return false;
  }
  for (std::size_t i = 0U; i < kMaxPlayerControllers; ++i) {
    const auto player = static_cast<std::uint8_t>(i);
    if ((binding_state().playerControllerEntities[i] == entity) ||
        (binding_state().playerControllers.get_controlled_entity(player) == entity)) {
      return true;
    }
  }
  return false;
}

void clear_player_controller_entity(core::Entity entity) noexcept {
  if (entity == core::kInvalidEntity) {
    return;
  }

  binding_state().playerControllers.on_entity_destroyed(entity);
  for (std::size_t i = 0U; i < kMaxPlayerControllers; ++i) {
    if (binding_state().playerControllerEntities[i] == entity) {
      binding_state().playerControllerEntities[i] = core::kInvalidEntity;
    }
  }
}

void reset_game_bindings() noexcept {
  std::snprintf(binding_state().gameMode, sizeof(binding_state().gameMode), "%s", "default");
  std::snprintf(binding_state().gameState, sizeof(binding_state().gameState), "%s", "startup");
  for (std::size_t i = 0U; i < kMaxPlayerControllers; ++i) {
    binding_state().playerControllerEntities[i] = core::kInvalidEntity;
  }
  binding_state().playerControllers.reset();
}

/// Binds the pipeline-owned state; nullptr restores the fallback.
void bind_game_state(GameBindingState *state) noexcept {
  g_boundState = state;
}

bool bindable_set_game_mode(const char *name) noexcept {
  return set_game_mode_name(name);
}

const char *bindable_get_game_state() noexcept { return binding_state().gameState; }

const char *bindable_get_game_mode() noexcept {
  if (runtime_bound()) {
    return runtime_binding().services->game_mode_name(runtime_binding().world);
  }
  return binding_state().gameMode;
}

bool bindable_set_game_state(const char *name) noexcept {
  return set_game_state_name(name);
}

int lua_engine_set_player_controller(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer player = lua_tointeger(state, 1);
  const lua_Integer entityHandle = lua_tointeger(state, 2);
  const auto maxPlayerIndex =
      static_cast<lua_Integer>(std::numeric_limits<std::uint8_t>::max());
  if ((player < 0) || (player > maxPlayerIndex) ||
      (entityHandle < 0)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Entity entity = runtime::kInvalidEntity;
  if (entityHandle != 0) {
    if (!read_entity(state, 2, &entity)) {
      lua_pushboolean(state, 0);
      return 1;
    }
  }

  lua_pushboolean(state,
                  set_player_controller_entity(
                      static_cast<std::uint8_t>(player), entity)
                      ? 1
                      : 0);
  return 1;
}

int lua_engine_get_player_controller(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  const lua_Integer player = lua_tointeger(state, 1);
  const auto maxPlayerIndex =
      static_cast<lua_Integer>(std::numeric_limits<std::uint8_t>::max());
  if ((player < 0) || (player > maxPlayerIndex)) {
    lua_pushnil(state);
    return 1;
  }

  const auto idx = static_cast<std::uint8_t>(player);
  const runtime::Entity entity = get_player_controller_entity(idx);
  if ((entity == runtime::kInvalidEntity) || !runtime_bound() ||
      !runtime_binding().services->is_alive(runtime_binding().world, entity)) {
    lua_pushinteger(state, 0);
    return 1;
  }

  push_entity_handle(state, entity);
  return 1;
}

} // namespace engine::scripting
