// Implements Lua animation bindings: engine.set_anim_param queues
// phase-safe parameter writes for the animation system, and animation
// events fired by the fixed-step update dispatch to registered handlers
// plus the global on_anim_event fallback.

#include "animation_bindings.h"

#include "binding_util.h"
#include "entity_handle.h"
#include "lua_state.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>

#include "engine/scripting/scripting.h"

namespace engine::scripting {

namespace {

constexpr std::size_t kMaxAnimEventHandlers = 8U;
int g_animEventHandlers[kMaxAnimEventHandlers] = {
    LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF,
    LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};

AnimationScriptBridge g_animationBridge{};

/// Carries one animation-event callback invocation into the trampoline.
struct AnimEventCallArgs final {
  core::Entity entity{};
  const char *eventName = nullptr;
  int handlerRef = LUA_NOREF;
};

/// Protected trampoline: resolves one handler (registry ref or the global
/// on_anim_event fallback), pushes the entity handle and event name, and
/// calls it, so metamethods and allocation failures stay catchable.
int anim_event_call_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<AnimEventCallArgs *>(lua_touserdata(state, 1));
  if (args->handlerRef != LUA_NOREF) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, args->handlerRef);
  } else {
    lua_getglobal(state, "on_anim_event");
  }
  if (lua_isfunction(state, -1) == 0) {
    return 0;
  }
  push_entity_handle(state, args->entity);
  lua_pushstring(state, args->eventName);
  lua_call(state, 2, 0);
  return 0;
}

} // namespace

void set_animation_script_bridge(
    const AnimationScriptBridge &bridge) noexcept {
  g_animationBridge = bridge;
}

// engine.set_anim_param(entity, name, value) → bool
// Queued and applied at the next fixed-step animation update, so scripts
// may call it from any callback without breaking phase gating.
int lua_engine_set_anim_param(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || (lua_isstring(state, 2) == 0) ||
      (lua_isnumber(state, 3) == 0)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 2);
  const float value = static_cast<float>(lua_tonumber(state, 3));
  const bool ok = (g_animationBridge.queueParam != nullptr) &&
                  g_animationBridge.queueParam(entity, name, value);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_on_anim_event_register(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  for (std::size_t i = 0U; i < kMaxAnimEventHandlers; ++i) {
    if (g_animEventHandlers[i] == LUA_NOREF) {
      lua_pushvalue(state, 1);
      g_animEventHandlers[i] = luaL_ref(state, LUA_REGISTRYINDEX);
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }
  }

  lua_pushnil(state);
  return 1;
}

int lua_engine_remove_anim_event_handler(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }

  const auto id = static_cast<std::size_t>(lua_tointeger(state, 1));
  if ((id < kMaxAnimEventHandlers) &&
      (g_animEventHandlers[id] != LUA_NOREF)) {
    luaL_unref(state, LUA_REGISTRYINDEX, g_animEventHandlers[id]);
    g_animEventHandlers[id] = LUA_NOREF;
  }
  return 0;
}

void clear_anim_event_handlers(lua_State *state) noexcept {
  if (state == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < kMaxAnimEventHandlers; ++i) {
    if (g_animEventHandlers[i] != LUA_NOREF) {
      luaL_unref(state, LUA_REGISTRYINDEX, g_animEventHandlers[i]);
      g_animEventHandlers[i] = LUA_NOREF;
    }
  }
}

void dispatch_anim_event_handlers() noexcept {
  lua_State *state = current_lua_state();
  if ((state == nullptr) || (g_animationBridge.firedEventCount == nullptr) ||
      (g_animationBridge.firedEventAt == nullptr)) {
    return;
  }
  const std::size_t eventCount = g_animationBridge.firedEventCount();

  for (std::size_t i = 0U; i < eventCount; ++i) {
    core::Entity entity{};
    const char *eventName = nullptr;
    if (!g_animationBridge.firedEventAt(i, &entity, &eventName) ||
        (eventName == nullptr)) {
      continue;
    }

    AnimEventCallArgs args{};
    args.entity = entity;
    args.eventName = eventName;

    for (std::size_t h = 0U; h < kMaxAnimEventHandlers; ++h) {
      if (g_animEventHandlers[h] == LUA_NOREF) {
        continue;
      }
      args.handlerRef = g_animEventHandlers[h];
      static_cast<void>(protected_engine_dispatch(state,
                                                  &anim_event_call_trampoline,
                                                  &args, 0,
                                                  "on_anim_event_handler"));
    }

    args.handlerRef = LUA_NOREF;
    static_cast<void>(protected_engine_dispatch(
        state, &anim_event_call_trampoline, &args, 0, "on_anim_event"));
  }
}

} // namespace engine::scripting
