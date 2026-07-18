// Declares private Lua game state and player binding helpers.

#pragma once

#include <cstdint>

#include "engine/core/entity.h"

struct lua_State;

namespace engine::scripting {

/// Lua binding: Lua engine.set_game_mode(name).
int lua_engine_set_game_mode(lua_State *state) noexcept;
/// Lua binding: Lua engine.get_game_mode().
int lua_engine_get_game_mode(lua_State *state) noexcept;
/// Lua binding: Lua engine.set_game_state(name).
int lua_engine_set_game_state(lua_State *state) noexcept;
/// Lua binding: Lua engine.get_game_state().
int lua_engine_get_game_state(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_mode_start().
int lua_engine_game_mode_start(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_mode_pause().
int lua_engine_game_mode_pause(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_mode_end().
int lua_engine_game_mode_end(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_mode_state().
int lua_engine_game_mode_state(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_mode_set_rule(key, value).
int lua_engine_game_mode_set_rule(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_mode_get_rule(key).
int lua_engine_game_mode_get_rule(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_mode_max_players([count]).
int lua_engine_game_mode_max_players(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_state_set_number(key, value).
int lua_engine_game_state_set_number(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_state_get_number(key).
int lua_engine_game_state_get_number(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_state_set_string(key, value).
int lua_engine_game_state_set_string(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_state_get_string(key).
int lua_engine_game_state_get_string(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_state_has(key).
int lua_engine_game_state_has(lua_State *state) noexcept;
/// Lua binding: Lua engine.game_state_clear().
int lua_engine_game_state_clear(lua_State *state) noexcept;

/// Stores a player-controller entity in the scripting-owned controller state.
bool set_player_controller_entity(std::uint8_t player,
                                  core::Entity entity) noexcept;
/// Returns the controlled entity for the requested player index.
core::Entity get_player_controller_entity(std::uint8_t player) noexcept;
/// Returns whether the entity is owned by any player controller slot.
bool is_player_controller_entity(core::Entity entity) noexcept;
/// Clears a destroyed entity from player-controller state.
void clear_player_controller_entity(core::Entity entity) noexcept;
/// Resets transient game binding state while preserving persistent game state.
void reset_game_bindings() noexcept;

/// Lua binding: Lua engine.set_player_controller(player, entity).
int lua_engine_set_player_controller(lua_State *state) noexcept;
/// Lua binding: Lua engine.get_player_controller(player).
int lua_engine_get_player_controller(lua_State *state) noexcept;

} // namespace engine::scripting
