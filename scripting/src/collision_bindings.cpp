// Owns Lua collision callback bindings for the Engine scripting system.

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

} // namespace

int lua_engine_on_collision_register(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1)) {
    lua_pushnil(state);
    lua_pushliteral(state, "on_collision_handler expects a function");
    return 2;
  }

  for (std::size_t i = 0U; i < kMaxCollisionHandlers; ++i) {
    if (g_collisionHandlers[i] == LUA_NOREF) {
      lua_pushvalue(state, 1);
      g_collisionHandlers[i] = luaL_ref(state, LUA_REGISTRYINDEX);
      g_handlersFullReported = false;
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }
  }

  char message[96] = {};
  std::snprintf(message, sizeof(message),
                "collision handler table full (%zu registered)",
                kMaxCollisionHandlers);
  if (!g_handlersFullReported) {
    g_handlersFullReported = true;
    core::log_message(core::LogLevel::Warning, "scripting", message);
  }
  lua_pushnil(state);
  lua_pushstring(state, message);
  return 2;
}

int lua_engine_remove_collision_handler(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }

  const auto id = static_cast<std::size_t>(lua_tointeger(state, 1));
  if ((id < kMaxCollisionHandlers) && (g_collisionHandlers[id] != LUA_NOREF)) {
    luaL_unref(state, LUA_REGISTRYINDEX, g_collisionHandlers[id]);
    g_collisionHandlers[id] = LUA_NOREF;
  }
  return 0;
}

void clear_collision_handlers(lua_State *state) noexcept {
  g_handlersFullReported = false;
  if (state == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < kMaxCollisionHandlers; ++i) {
    if (g_collisionHandlers[i] != LUA_NOREF) {
      luaL_unref(state, LUA_REGISTRYINDEX, g_collisionHandlers[i]);
      g_collisionHandlers[i] = LUA_NOREF;
    }
  }
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

} // namespace engine::scripting
