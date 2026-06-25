// Implements Lua entity pool bindings for the Engine scripting system.

#include "entity_pool_bindings.h"

#include "entity_handle.h"
#include "runtime_binding.h"

#include <cstddef>

#include "engine/runtime/entity_pool.h"

namespace engine::scripting {
namespace {

constexpr std::size_t kMaxEntityPools = 16U;
runtime::EntityPool g_entityPools[kMaxEntityPools]{};
std::size_t g_entityPoolCount = 0U;

/// Creates a fixed-size runtime entity pool from Lua.
int lua_engine_pool_create(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !lua_isinteger(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  const lua_Integer count = lua_tointeger(state, 1);
  if ((count <= 0) ||
      (static_cast<std::size_t>(count) > runtime::EntityPool::kMaxPoolSize)) {
    lua_pushnil(state);
    return 1;
  }

  if (g_entityPoolCount >= kMaxEntityPools) {
    lua_pushnil(state);
    return 1;
  }

  runtime::EntityPool &pool = g_entityPools[g_entityPoolCount];
  if (!pool.init(runtime_binding().world, static_cast<std::size_t>(count))) {
    lua_pushnil(state);
    return 1;
  }

  const std::size_t poolId = g_entityPoolCount;
  ++g_entityPoolCount;
  lua_pushinteger(state, static_cast<lua_Integer>(poolId));
  return 1;
}

/// Acquires an entity from a Lua-created entity pool.
int lua_engine_pool_spawn(lua_State *state) noexcept {
  if (!lua_isinteger(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  const lua_Integer poolId = lua_tointeger(state, 1);
  if ((poolId < 0) || (static_cast<std::size_t>(poolId) >= g_entityPoolCount)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity entity =
      g_entityPools[static_cast<std::size_t>(poolId)].acquire();
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  push_entity_handle(state, entity);
  return 1;
}

/// Releases an entity back to a Lua-created entity pool.
int lua_engine_pool_release(lua_State *state) noexcept {
  if (!lua_isinteger(state, 1) || !lua_isinteger(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer poolId = lua_tointeger(state, 1);
  if ((poolId < 0) || (static_cast<std::size_t>(poolId) >= g_entityPoolCount)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Entity entity{};
  if (!read_entity(state, 2, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const bool ok =
      g_entityPools[static_cast<std::size_t>(poolId)].release(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

} // namespace

void register_entity_pool_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_pool_create);
  lua_setfield(state, -2, "pool_create");
  lua_pushcfunction(state, &lua_engine_pool_spawn);
  lua_setfield(state, -2, "pool_spawn");
  lua_pushcfunction(state, &lua_engine_pool_release);
  lua_setfield(state, -2, "pool_release");
}

void reset_entity_pool_bindings() noexcept {
  for (std::size_t i = 0U; i < kMaxEntityPools; ++i) {
    g_entityPools[i] = runtime::EntityPool{};
  }
  g_entityPoolCount = 0U;
}

} // namespace engine::scripting
