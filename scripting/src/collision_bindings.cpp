// Owns Lua collision and trigger callback bindings for the Engine scripting
// system.

#include "collision_bindings.h"

#include "binding_util.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <array>
#include <cstdio>

#include "engine/core/logging.h"

namespace engine::scripting {
namespace {

// Global collision handlers a run may register. Entity scripts have no
// collision hook of their own yet, so every behaviour that reacts to
// contacts registers one here; 8 was met by a game with that many such
// behaviours. Unity, Godot and Unreal bound listeners only by memory.
constexpr std::size_t kMaxCollisionHandlers = 64U;

/// A table of empty handler slots.
std::array<int, kMaxCollisionHandlers> empty_handlers() noexcept {
  std::array<int, kMaxCollisionHandlers> handlers =
      std::array<int, kMaxCollisionHandlers>();
  handlers.fill(LUA_NOREF);
  return handlers;
}

std::array<int, kMaxCollisionHandlers> g_collisionHandlers = empty_handlers();
/// Set by the first refusal of a full table and cleared once a register
/// succeeds again, so a script retrying every frame logs one line.
bool g_handlersFullReported = false;

// Trigger handlers: a table of their own, the same size and policy.
std::array<int, kMaxCollisionHandlers> g_triggerHandlers = empty_handlers();
bool g_triggerHandlersFullReported = false;

/// Carries one collision callback invocation into the protected trampoline.
struct CollisionCallArgs final {
  PushEntityHandleFn pushEntityHandle = nullptr;
  core::Entity entityA{};
  core::Entity entityB{};
  int handlerRef = LUA_NOREF;
};

/// Protected trampoline: resolves one handler (registry ref or the global
/// on_collision fallback), pushes both entity handles, and calls it, so
/// metamethods and allocation failures stay catchable. The handles are
/// pushed from the pair's recorded identities — never re-resolved from an
/// index — so a participant destroyed by an earlier handler in the same
/// dispatch pushes as nil rather than as whatever entity now occupies its
/// recycled index.
int collision_call_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<CollisionCallArgs *>(lua_touserdata(state, 1));
  if (args->handlerRef != LUA_NOREF) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, args->handlerRef);
  } else {
    lua_getglobal(state, "on_collision");
  }
  if (lua_isfunction(state, -1) == 0) {
    return 0;
  }
  args->pushEntityHandle(state, args->entityA);
  args->pushEntityHandle(state, args->entityB);
  lua_call(state, 2, 0);
  return 0;
}

/// Carries one trigger event into the protected trampoline.
struct TriggerCallArgs final {
  PushEntityHandleFn pushEntityHandle = nullptr;
  core::Entity trigger{};
  core::Entity other{};
  bool entered = false;
  int handlerRef = LUA_NOREF;
};

/// Protected trampoline for one trigger handler: pushes the recorded
/// identities and the phase name, so a stale participant arrives as nil.
int trigger_call_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<TriggerCallArgs *>(lua_touserdata(state, 1));
  lua_rawgeti(state, LUA_REGISTRYINDEX, args->handlerRef);
  if (lua_isfunction(state, -1) == 0) {
    return 0;
  }
  args->pushEntityHandle(state, args->trigger);
  args->pushEntityHandle(state, args->other);
  if (args->entered) {
    lua_pushliteral(state, "enter");
  } else {
    lua_pushliteral(state, "exit");
  }
  lua_call(state, 3, 0);
  return 0;
}

/// Registers the function at stack index 1 in `handlers`, returning its id
/// or nil and a reason; the first refusal of a full table logs a Warning.
int register_handler(lua_State *state,
                     std::array<int, kMaxCollisionHandlers> &handlers,
                     bool *fullReported, const char *apiName,
                     const char *tableName) noexcept {
  if (!lua_isfunction(state, 1)) {
    lua_pushnil(state);
    lua_pushfstring(state, "%s expects a function", apiName);
    return 2;
  }

  for (std::size_t i = 0U; i < kMaxCollisionHandlers; ++i) {
    if (handlers[i] == LUA_NOREF) {
      lua_pushvalue(state, 1);
      handlers[i] = luaL_ref(state, LUA_REGISTRYINDEX);
      *fullReported = false;
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }
  }

  char message[96] = {};
  std::snprintf(message, sizeof(message),
                "%s handler table full (%zu registered)", tableName,
                kMaxCollisionHandlers);
  if (!*fullReported) {
    *fullReported = true;
    core::log_message(core::LogLevel::Warning, "scripting", message);
  }
  lua_pushnil(state);
  lua_pushstring(state, message);
  return 2;
}

/// Releases handler `id` (the integer at stack index 1) if it is live.
void remove_handler(lua_State *state,
                    std::array<int, kMaxCollisionHandlers> &handlers) noexcept {
  if (!lua_isnumber(state, 1)) {
    return;
  }
  const auto id = static_cast<std::size_t>(lua_tointeger(state, 1));
  if ((id < kMaxCollisionHandlers) && (handlers[id] != LUA_NOREF)) {
    luaL_unref(state, LUA_REGISTRYINDEX, handlers[id]);
    handlers[id] = LUA_NOREF;
  }
}

/// Releases every live handler in `handlers`.
void clear_handlers(lua_State *state,
                    std::array<int, kMaxCollisionHandlers> &handlers) noexcept {
  if (state == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < kMaxCollisionHandlers; ++i) {
    if (handlers[i] != LUA_NOREF) {
      luaL_unref(state, LUA_REGISTRYINDEX, handlers[i]);
      handlers[i] = LUA_NOREF;
    }
  }
}

} // namespace

int lua_engine_on_collision_register(lua_State *state) noexcept {
  return register_handler(state, g_collisionHandlers, &g_handlersFullReported,
                          "on_collision_handler", "collision");
}

int lua_engine_remove_collision_handler(lua_State *state) noexcept {
  remove_handler(state, g_collisionHandlers);
  return 0;
}

void clear_collision_handlers(lua_State *state) noexcept {
  g_handlersFullReported = false;
  clear_handlers(state, g_collisionHandlers);
}

int lua_engine_on_trigger_register(lua_State *state) noexcept {
  return register_handler(state, g_triggerHandlers,
                          &g_triggerHandlersFullReported, "on_trigger_handler",
                          "trigger");
}

int lua_engine_remove_trigger_handler(lua_State *state) noexcept {
  remove_handler(state, g_triggerHandlers);
  return 0;
}

void clear_trigger_handlers(lua_State *state) noexcept {
  g_triggerHandlersFullReported = false;
  clear_handlers(state, g_triggerHandlers);
}

void dispatch_collision_handlers(lua_State *state,
                                 const core::Entity *pairData,
                                 std::size_t pairCount,
                                 PushEntityHandleFn pushEntityHandle) noexcept {
  if ((state == nullptr) || (pairData == nullptr) || (pairCount == 0U) ||
      (pushEntityHandle == nullptr)) {
    return;
  }

  for (std::size_t i = 0U; i < pairCount; ++i) {
    CollisionCallArgs args{};
    args.pushEntityHandle = pushEntityHandle;
    args.entityA = pairData[i * 2U];
    args.entityB = pairData[i * 2U + 1U];

    for (std::size_t h = 0U; h < kMaxCollisionHandlers; ++h) {
      if (g_collisionHandlers[h] == LUA_NOREF) {
        continue;
      }
      args.handlerRef = g_collisionHandlers[h];
      static_cast<void>(protected_engine_dispatch(state,
                                                  &collision_call_trampoline,
                                                  &args, 0,
                                                  "on_collision_handler"));
    }

    args.handlerRef = LUA_NOREF;
    static_cast<void>(protected_engine_dispatch(
        state, &collision_call_trampoline, &args, 0, "on_collision"));
  }
}

void dispatch_trigger_handlers(lua_State *state, const core::Entity *pairData,
                               const std::uint8_t *entered,
                               std::size_t eventCount,
                               PushEntityHandleFn pushEntityHandle) noexcept {
  if ((state == nullptr) || (pairData == nullptr) || (entered == nullptr) ||
      (eventCount == 0U) || (pushEntityHandle == nullptr)) {
    return;
  }

  for (std::size_t i = 0U; i < eventCount; ++i) {
    TriggerCallArgs args{};
    args.pushEntityHandle = pushEntityHandle;
    args.trigger = pairData[i * 2U];
    args.other = pairData[(i * 2U) + 1U];
    args.entered = entered[i] != 0U;

    for (std::size_t h = 0U; h < kMaxCollisionHandlers; ++h) {
      if (g_triggerHandlers[h] == LUA_NOREF) {
        continue;
      }
      args.handlerRef = g_triggerHandlers[h];
      static_cast<void>(protected_engine_dispatch(
          state, &trigger_call_trampoline, &args, 0, "on_trigger_handler"));
    }
  }
}

} // namespace engine::scripting
