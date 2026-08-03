// Owns Lua collision callback bindings for the Engine scripting system.

#include "collision_bindings.h"

#include "binding_util.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace engine::scripting {
namespace {

constexpr std::size_t kMaxCollisionHandlers = 8U;
int g_collisionHandlers[kMaxCollisionHandlers] = {
    LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF,
    LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};

/// Carries one collision callback invocation into the protected trampoline.
struct CollisionCallArgs final {
  PushEntityHandleFromIndexFn pushEntityHandleFromIndex = nullptr;
  std::uint32_t entityIndexA = 0U;
  std::uint32_t entityIndexB = 0U;
  int handlerRef = LUA_NOREF;
};

/// Protected trampoline: resolves one handler (registry ref or the global
/// on_collision fallback), pushes both entity handles, and calls it, so
/// metamethods and allocation failures stay catchable.
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
  args->pushEntityHandleFromIndex(state, args->entityIndexA);
  args->pushEntityHandleFromIndex(state, args->entityIndexB);
  lua_call(state, 2, 0);
  return 0;
}

} // namespace

int lua_engine_on_collision_register(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  for (std::size_t i = 0U; i < kMaxCollisionHandlers; ++i) {
    if (g_collisionHandlers[i] == LUA_NOREF) {
      lua_pushvalue(state, 1);
      g_collisionHandlers[i] = luaL_ref(state, LUA_REGISTRYINDEX);
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }
  }

  lua_pushnil(state);
  return 1;
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

void dispatch_collision_handlers(
    lua_State *state, const std::uint32_t *pairData, std::size_t pairCount,
    PushEntityHandleFromIndexFn pushEntityHandleFromIndex) noexcept {
  if ((state == nullptr) || (pairData == nullptr) || (pairCount == 0U) ||
      (pushEntityHandleFromIndex == nullptr)) {
    return;
  }

  for (std::size_t i = 0U; i < pairCount; ++i) {
    CollisionCallArgs args{};
    args.pushEntityHandleFromIndex = pushEntityHandleFromIndex;
    args.entityIndexA = pairData[i * 2U];
    args.entityIndexB = pairData[i * 2U + 1U];

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
