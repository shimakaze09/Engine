// Implements audio Lua bindings (sound loading, playback, master volume)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "audio_bindings.h"

#include "binding_util.h"
#include "deferred_mutations.h"
#include "entity_handle.h"
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

int lua_engine_load_sound(lua_State *state) noexcept {
  if (!lua_isstring(state, 1)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushinteger(state, 0);
    return 1;
  }
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->load_sound == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  lua_pushinteger(state,
                  static_cast<lua_Integer>(runtime_binding().services->load_sound(path)));
  return 1;
}

int lua_engine_unload_sound(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->unload_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    runtime_binding().services->unload_sound(id);
  }
  return 0;
}

int lua_engine_play_sound(lua_State *state) noexcept {
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->play_sound == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
  float volume = 1.0F;
  float pitch = 1.0F;
  bool loop = false;
  if ((lua_gettop(state) >= 2) && lua_isnumber(state, 2)) {
    volume = static_cast<float>(lua_tonumber(state, 2));
  }
  if ((lua_gettop(state) >= 3) && lua_isnumber(state, 3)) {
    pitch = static_cast<float>(lua_tonumber(state, 3));
  }
  if (lua_gettop(state) >= 4) {
    loop = lua_toboolean(state, 4) != 0;
  }
  const bool ok = runtime_binding().services->play_sound(id, volume, pitch, loop);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_stop_sound(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->stop_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    runtime_binding().services->stop_sound(id);
  }
  return 0;
}

int lua_engine_stop_all_sounds(lua_State *state) noexcept {
  static_cast<void>(state);
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->stop_all_sounds != nullptr)) {
    runtime_binding().services->stop_all_sounds();
  }
  return 0;
}

int lua_engine_set_master_volume(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_master_volume != nullptr)) {
    const auto vol = static_cast<float>(lua_tonumber(state, 1));
    runtime_binding().services->set_master_volume(vol);
  }
  return 0;
}

// --- Transform: rotation and scale ---

} // namespace

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_audio_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_load_sound);
  lua_setfield(state, -2, "load_sound");
  lua_pushcfunction(state, &lua_engine_unload_sound);
  lua_setfield(state, -2, "unload_sound");
  lua_pushcfunction(state, &lua_engine_play_sound);
  lua_setfield(state, -2, "play_sound");
  lua_pushcfunction(state, &lua_engine_stop_sound);
  lua_setfield(state, -2, "stop_sound");
  lua_pushcfunction(state, &lua_engine_stop_all_sounds);
  lua_setfield(state, -2, "stop_all_sounds");
  lua_pushcfunction(state, &lua_engine_set_master_volume);
  lua_setfield(state, -2, "set_master_volume");
}

} // namespace engine::scripting
