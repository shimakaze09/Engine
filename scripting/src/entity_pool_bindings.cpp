// Implements Lua entity pool bindings for the Engine scripting system.

#include "entity_pool_bindings.h"

#include "entity_handle.h"
#include "runtime_binding.h"

#include <cstddef>
#include <cstdint>

#include "engine/runtime/entity_pool.h"
#include "engine/runtime/world.h"

namespace engine::scripting {
namespace {

constexpr std::size_t kMaxEntityPools = 16U;
runtime::EntityPool g_entityPools[kMaxEntityPools]{};
std::size_t g_entityPoolCount = 0U;

// Lua-visible pool id layout (#93b): slot index in the low bits, the
// creating world's content epoch above it, mirroring the entity-handle
// scheme in entity_handle.cpp. reset_entity_pool_bindings() now runs on
// every scene transition (engine_pipeline's process_pending_scene_op) to
// reclaim pool slots instead of leaking one per transition; the epoch field
// is what keeps that reclaim safe — a poolId a script held across the
// transition decodes to the epoch it was created under, so it is rejected
// rather than silently aliasing a same-numbered pool the new scene creates.
constexpr unsigned kPoolSlotBits = 8U;
constexpr std::uint64_t kPoolSlotMask = (1ULL << kPoolSlotBits) - 1ULL;
constexpr unsigned kPoolEpochShift = kPoolSlotBits;
static_assert(kMaxEntityPools <= kPoolSlotMask,
              "slot field too small for the configured pool capacity");

/// Encodes a pool slot plus the current world epoch into a Lua pool id;
/// zero (nil) on no bound world.
bool encode_pool_id(std::size_t slot, lua_Integer *outId) noexcept {
  if ((outId == nullptr) || (runtime_binding().world == nullptr)) {
    return false;
  }
  const auto epoch =
      static_cast<std::uint64_t>(runtime_binding().world->content_epoch());
  const std::uint64_t encoded =
      (epoch << kPoolEpochShift) | (static_cast<std::uint64_t>(slot) + 1ULL);
  *outId = static_cast<lua_Integer>(encoded);
  return true;
}

/// Decodes a Lua pool id into a live slot index; false when malformed, out
/// of the currently allocated range, or stamped with a stale world epoch.
bool decode_pool_id(lua_Integer rawId, std::size_t *outSlot) noexcept {
  if ((outSlot == nullptr) || (rawId <= 0) ||
      (runtime_binding().world == nullptr)) {
    return false;
  }
  const auto encoded = static_cast<std::uint64_t>(rawId);
  const std::uint64_t encodedSlot = encoded & kPoolSlotMask;
  const std::uint64_t encodedEpoch = encoded >> kPoolEpochShift;
  const auto currentEpoch =
      static_cast<std::uint64_t>(runtime_binding().world->content_epoch());
  if ((encodedSlot == 0ULL) || (encodedEpoch != currentEpoch)) {
    return false;
  }
  const std::size_t slot = static_cast<std::size_t>(encodedSlot - 1ULL);
  if (slot >= g_entityPoolCount) {
    return false;
  }
  *outSlot = slot;
  return true;
}

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

  lua_Integer poolId = 0;
  if (!encode_pool_id(g_entityPoolCount, &poolId)) {
    pool = runtime::EntityPool{};
    lua_pushnil(state);
    return 1;
  }
  ++g_entityPoolCount;
  lua_pushinteger(state, poolId);
  return 1;
}

/// Acquires an entity from a Lua-created entity pool.
int lua_engine_pool_spawn(lua_State *state) noexcept {
  if (!lua_isinteger(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  std::size_t slot = 0U;
  if (!decode_pool_id(lua_tointeger(state, 1), &slot)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity entity = g_entityPools[slot].acquire();
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

  std::size_t slot = 0U;
  if (!decode_pool_id(lua_tointeger(state, 1), &slot)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Entity entity{};
  if (!read_entity(state, 2, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const bool ok = g_entityPools[slot].release(entity);
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

std::size_t pool_slot_count() noexcept { return g_entityPoolCount; }

} // namespace engine::scripting
