// Implements audio Lua bindings (sound loading, playback, master volume)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "audio_bindings.h"

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

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/scripting/runtime_services.h"

namespace engine::scripting {

namespace {

int lua_engine_load_sound(lua_State *state) noexcept {
  if (!lua_isstring(state, 1)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if ((path == nullptr) || !script_path_in_jail(path, "load_sound")) {
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

/// Hands a fire-and-forget audio call to the reload scope when one is
/// open. True when the caller performs the call itself; false when it was
/// held or refused (a refused call is logged and does nothing).
bool audio_call_runs_now(const StagedAudioOp &staged) noexcept {
  switch (reload_staging(ReloadEffect::Audio)) {
  case ReloadStaging::None:
    return true;
  case ReloadStaging::Staged:
    reload_hold_audio(staged);
    return false;
  case ReloadStaging::Refused:
    core::log_message(core::LogLevel::Warning, "scripting",
                      "audio call refused: the hot reload holds no more");
    return false;
  }
  return false;
}

/// Same for a call that reports success: true when the call ran or was
/// held, false when the scope refused it or the call itself failed.
template <typename Call>
bool audio_call_ok(const StagedAudioOp &staged, Call call) noexcept {
  switch (reload_staging(ReloadEffect::Audio)) {
  case ReloadStaging::None:
    return call();
  case ReloadStaging::Staged:
    reload_hold_audio(staged);
    return true;
  case ReloadStaging::Refused:
    core::log_message(core::LogLevel::Warning, "scripting",
                      "audio call refused: the hot reload holds no more");
    return false;
  }
  return false;
}

int lua_engine_unload_sound(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->unload_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    StagedAudioOp staged{};
    staged.kind = StagedAudioOp::Kind::UnloadSound;
    staged.id = id;
    if (audio_call_runs_now(staged)) {
      runtime_binding().services->unload_sound(id);
    }
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
  if (!read_optional_finite_number_arg(state, 2, 1.0F, &volume) ||
      !read_optional_finite_number_arg(state, 3, 1.0F, &pitch)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (lua_gettop(state) >= 4) {
    loop = lua_toboolean(state, 4) != 0;
  }
  StagedAudioOp staged{};
  staged.kind = StagedAudioOp::Kind::PlaySound;
  staged.id = id;
  staged.a = volume;
  staged.b = pitch;
  staged.flag = loop;
  const bool ok = audio_call_ok(staged, [&]() noexcept {
    return runtime_binding().services->play_sound(id, volume, pitch, loop);
  });
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_stop_sound(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->stop_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    StagedAudioOp staged{};
    staged.kind = StagedAudioOp::Kind::StopSound;
    staged.id = id;
    if (audio_call_runs_now(staged)) {
      runtime_binding().services->stop_sound(id);
    }
  }
  return 0;
}

// engine.play_sound_at(sound, x, y, z [, volume]) → bool
// Spatialized fire-and-forget one-shot on the sfx bus.
int lua_engine_play_sound_at(lua_State *state) noexcept {
  math::Vec3 position{};
  float volume = 1.0F;
  if (!lua_isnumber(state, 1) || !read_vec3_args(state, 2, &position) ||
      !read_optional_finite_number_arg(state, 5, 1.0F, &volume)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  bool ok = false;
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->play_sound_at != nullptr)) {
    const auto soundId = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    StagedAudioOp staged{};
    staged.kind = StagedAudioOp::Kind::PlaySoundAt;
    staged.id = soundId;
    staged.a = position.x;
    staged.b = position.y;
    staged.c = position.z;
    staged.d = volume;
    ok = audio_call_ok(staged, [&]() noexcept {
      return runtime_binding().services->play_sound_at(
          soundId, position.x, position.y, position.z, volume);
    });
  }
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.set_bus_volume(bus, volume): bus is "master", "music", or "sfx".
int lua_engine_set_bus_volume(lua_State *state) noexcept {
  float volume = 0.0F;
  if (!lua_isstring(state, 1) || !read_finite_number_arg(state, 2, &volume)) {
    return 0;
  }
  const char *busName = lua_tostring(state, 1);
  std::uint32_t bus = 0U;
  if (std::strcmp(busName, "music") == 0) {
    bus = 1U;
  } else if (std::strcmp(busName, "sfx") == 0) {
    bus = 2U;
  } else if (std::strcmp(busName, "master") != 0) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->set_bus_volume != nullptr)) {
    StagedAudioOp staged{};
    staged.kind = StagedAudioOp::Kind::SetBusVolume;
    staged.id = bus;
    staged.a = volume;
    if (audio_call_runs_now(staged)) {
      runtime_binding().services->set_bus_volume(bus, volume);
    }
  }
  return 0;
}

// engine.play_music(path [, volume, loop]) → bool (streams on the music bus).
int lua_engine_play_music(lua_State *state) noexcept {
  if (!lua_isstring(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if ((path == nullptr) || !script_path_in_jail(path, "play_music")) {
    lua_pushboolean(state, 0);
    return 1;
  }
  float volume = 1.0F;
  if (!read_optional_finite_number_arg(state, 2, 1.0F, &volume)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  bool ok = false;
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->play_music != nullptr)) {
    const bool loop = (lua_isboolean(state, 3) == 0) ||
                      (lua_toboolean(state, 3) != 0);
    StagedAudioOp staged{};
    staged.kind = StagedAudioOp::Kind::PlayMusic;
    staged.a = volume;
    staged.flag = loop;
    // The jail already bounded the path; a longer one is refused under
    // reload rather than replayed truncated.
    if (reload_transaction_open() &&
        (std::strlen(path) > StagedAudioOp::kMaxPathLength)) {
      ok = false;
    } else {
      std::snprintf(staged.path, sizeof(staged.path), "%s", path);
      ok = audio_call_ok(staged, [&]() noexcept {
        return runtime_binding().services->play_music(path, volume, loop);
      });
    }
  }
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.stop_music() stops the streamed track.
int lua_engine_stop_music(lua_State *state) noexcept {
  static_cast<void>(state);
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->stop_music != nullptr)) {
    StagedAudioOp staged{};
    staged.kind = StagedAudioOp::Kind::StopMusic;
    if (audio_call_runs_now(staged)) {
      runtime_binding().services->stop_music();
    }
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
  lua_pushcfunction(state, &lua_engine_play_sound_at);
  lua_setfield(state, -2, "play_sound_at");
  lua_pushcfunction(state, &lua_engine_set_bus_volume);
  lua_setfield(state, -2, "set_bus_volume");
  lua_pushcfunction(state, &lua_engine_play_music);
  lua_setfield(state, -2, "play_music");
  lua_pushcfunction(state, &lua_engine_stop_music);
  lua_setfield(state, -2, "stop_music");
}

} // namespace engine::scripting
