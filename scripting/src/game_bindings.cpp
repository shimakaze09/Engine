// Implements private Lua game state and player binding helpers.

#include "game_bindings.h"

extern "C" {
#include "lua.h"
}

#include <cstddef>
#include <cstdio>

#include "engine/runtime/game_mode.h"
#include "engine/runtime/game_state.h"
#include "engine/runtime/player_controller.h"
#include "engine/runtime/world.h"
#include "runtime_binding.h"

namespace engine::scripting {
namespace {

char g_gameMode[64] = "default";
char g_gameState[64] = "startup";
constexpr std::size_t kMaxPlayerControllers = 4U;
core::Entity g_playerControllerEntities[kMaxPlayerControllers]{};
runtime::GameState g_persistentGameState{};
runtime::PlayerControllerArray g_playerControllers{};

/// Stores the current game mode name in scripting and the bound world.
bool set_game_mode_name(const char *name) noexcept {
  if (name == nullptr) {
    return false;
  }
  std::snprintf(g_gameMode, sizeof(g_gameMode), "%s", name);
  if (runtime_binding().world != nullptr) {
    std::snprintf(runtime_binding().world->game_mode().name,
                  runtime::GameMode::kMaxNameLength, "%s", name);
  }
  return true;
}

/// Stores the current scripting game-state label.
bool set_game_state_name(const char *name) noexcept {
  if (name == nullptr) {
    return false;
  }
  std::snprintf(g_gameState, sizeof(g_gameState), "%s", name);
  return true;
}

} // namespace

int lua_engine_set_game_mode(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, set_game_mode_name(name) ? 1 : 0);
  return 1;
}

int lua_engine_get_game_mode(lua_State *state) noexcept {
  if (runtime_binding().world != nullptr) {
    lua_pushstring(state, runtime_binding().world->game_mode().name);
  } else {
    lua_pushstring(state, g_gameMode);
  }
  return 1;
}

int lua_engine_set_game_state(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, set_game_state_name(name) ? 1 : 0);
  return 1;
}

int lua_engine_get_game_state(lua_State *state) noexcept {
  lua_pushstring(state, g_gameState);
  return 1;
}

int lua_engine_game_mode_start(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().world->game_mode().start() ? 1 : 0);
  return 1;
}

int lua_engine_game_mode_pause(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().world->game_mode().pause() ? 1 : 0);
  return 1;
}

int lua_engine_game_mode_end(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().world->game_mode().end() ? 1 : 0);
  return 1;
}

int lua_engine_game_mode_state(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushstring(state, "none");
    return 1;
  }
  using S = runtime::GameMode::State;
  switch (runtime_binding().world->game_mode().state) {
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
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *key = lua_tostring(state, 1);
  const char *value = lua_tostring(state, 2);
  if (key == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(
      state, runtime_binding().world->game_mode().set_rule(key, value) ? 1 : 0);
  return 1;
}

int lua_engine_game_mode_get_rule(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  const char *key = lua_tostring(state, 1);
  const char *value = runtime_binding().world->game_mode().get_rule(key);
  if (value != nullptr) {
    lua_pushstring(state, value);
  } else {
    lua_pushnil(state);
  }
  return 1;
}

int lua_engine_game_mode_max_players(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushinteger(state, 0);
    return 1;
  }
  if ((lua_gettop(state) >= 1) && (lua_isnumber(state, 1) != 0)) {
    const auto n = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    runtime_binding().world->game_mode().maxPlayers = n;
  }
  lua_pushinteger(
      state,
      static_cast<lua_Integer>(runtime_binding().world->game_mode().maxPlayers));
  return 1;
}

int lua_engine_game_state_set_number(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  if ((key == nullptr) || (lua_isnumber(state, 2) == 0)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_persistentGameState.set_number(
                             key, static_cast<float>(lua_tonumber(state, 2)))
                             ? 1
                             : 0);
  return 1;
}

int lua_engine_game_state_get_number(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  lua_pushnumber(
      state, static_cast<lua_Number>(g_persistentGameState.get_number(key)));
  return 1;
}

int lua_engine_game_state_set_string(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  const char *value = lua_tostring(state, 2);
  if (key == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_persistentGameState.set_string(key, value) ? 1 : 0);
  return 1;
}

int lua_engine_game_state_get_string(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  const char *value = g_persistentGameState.get_string(key);
  if (value != nullptr) {
    lua_pushstring(state, value);
  } else {
    lua_pushnil(state);
  }
  return 1;
}

int lua_engine_game_state_has(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  lua_pushboolean(state, g_persistentGameState.has(key) ? 1 : 0);
  return 1;
}

int lua_engine_game_state_clear(lua_State *state) noexcept {
  static_cast<void>(state);
  g_persistentGameState.clear();
  return 0;
}

bool set_player_controller_entity(std::uint8_t player,
                                  core::Entity entity) noexcept {
  if (player >= kMaxPlayerControllers) {
    return false;
  }
  g_playerControllerEntities[player] = entity;
  g_playerControllers.set_controlled_entity(player, entity);
  return true;
}

core::Entity get_player_controller_entity(std::uint8_t player) noexcept {
  if (player >= kMaxPlayerControllers) {
    return core::kInvalidEntity;
  }
  return g_playerControllers.get_controlled_entity(player);
}

bool is_player_controller_entity(core::Entity entity) noexcept {
  if (entity == core::kInvalidEntity) {
    return false;
  }
  for (std::size_t i = 0U; i < kMaxPlayerControllers; ++i) {
    const auto player = static_cast<std::uint8_t>(i);
    if ((g_playerControllerEntities[i] == entity) ||
        (g_playerControllers.get_controlled_entity(player) == entity)) {
      return true;
    }
  }
  return false;
}

void clear_player_controller_entity(core::Entity entity) noexcept {
  if (entity == core::kInvalidEntity) {
    return;
  }

  g_playerControllers.on_entity_destroyed(entity);
  for (std::size_t i = 0U; i < kMaxPlayerControllers; ++i) {
    if (g_playerControllerEntities[i] == entity) {
      g_playerControllerEntities[i] = core::kInvalidEntity;
    }
  }
}

void reset_game_bindings() noexcept {
  std::snprintf(g_gameMode, sizeof(g_gameMode), "%s", "default");
  std::snprintf(g_gameState, sizeof(g_gameState), "%s", "startup");
  for (std::size_t i = 0U; i < kMaxPlayerControllers; ++i) {
    g_playerControllerEntities[i] = core::kInvalidEntity;
  }
  g_playerControllers.reset();
}

bool bindable_set_game_mode(const char *name) noexcept {
  return set_game_mode_name(name);
}

const char *bindable_get_game_state() noexcept { return g_gameState; }

const char *bindable_get_game_mode() noexcept {
  if (runtime_binding().world != nullptr) {
    return runtime_binding().world->game_mode().name;
  }
  return g_gameMode;
}

bool bindable_set_game_state(const char *name) noexcept {
  return set_game_state_name(name);
}

} // namespace engine::scripting
