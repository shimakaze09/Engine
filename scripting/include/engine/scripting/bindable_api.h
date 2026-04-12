#pragma once
// Bindable API — thin accessors for Lua binding generation.
// Annotated with // LUA_BIND: for use with tools/binding_generator.
// Implementations live in scripting.cpp (same TU as the internal globals).

#include <cstdint>

namespace engine::scripting {

// LUA_BIND: delta_time() -> float
float bindable_delta_time() noexcept;

// LUA_BIND: elapsed_time() -> float
float bindable_elapsed_time() noexcept;

// LUA_BIND: frame_count() -> int
int bindable_frame_count() noexcept;

// LUA_BIND: get_entity_count() -> int
int bindable_get_entity_count() noexcept;

// LUA_BIND: is_god_mode() -> bool
bool bindable_is_god_mode() noexcept;

// LUA_BIND: is_noclip() -> bool
bool bindable_is_noclip() noexcept;

// LUA_BIND: is_gamepad_connected() -> bool
bool bindable_is_gamepad_connected() noexcept;

// LUA_BIND: is_key_down(scancode: int) -> bool
bool bindable_is_key_down(int scancode) noexcept;

// LUA_BIND: is_key_pressed(scancode: int) -> bool
bool bindable_is_key_pressed(int scancode) noexcept;

// LUA_BIND: is_gamepad_button_down(button: int) -> bool
bool bindable_is_gamepad_button_down(int button) noexcept;

// LUA_BIND: is_action_down(name: string) -> bool
bool bindable_is_action_down(const char *name) noexcept;

// LUA_BIND: is_action_pressed(name: string) -> bool
bool bindable_is_action_pressed(const char *name) noexcept;

// LUA_BIND: get_action_value(name: string) -> float
float bindable_get_action_value(const char *name) noexcept;

// LUA_BIND: get_axis_value(name: string) -> float
float bindable_get_axis_value(const char *name) noexcept;

// LUA_BIND: set_game_mode(name: string) -> bool
bool bindable_set_game_mode(const char *name) noexcept;

// LUA_BIND: get_game_state() -> string
const char *bindable_get_game_state() noexcept;

// LUA_BIND: get_game_mode() -> string
const char *bindable_get_game_mode() noexcept;

// LUA_BIND: set_game_state(name: string) -> bool
bool bindable_set_game_state(const char *name) noexcept;

// LUA_BIND: is_alive(entity: uint32) -> bool
bool bindable_is_alive(std::uint32_t entity) noexcept;

// LUA_BIND: has_light(entity: uint32) -> bool
bool bindable_has_light(std::uint32_t entity) noexcept;

// LUA_BIND: set_camera_fov(fov: float) -> void
void bindable_set_camera_fov(float fov) noexcept;

// LUA_BIND: set_master_volume(volume: float) -> void
void bindable_set_master_volume(float volume) noexcept;

// LUA_BIND: stop_all_sounds() -> void
void bindable_stop_all_sounds() noexcept;

} // namespace engine::scripting
