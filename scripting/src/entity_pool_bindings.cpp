// Implements Lua entity pool bindings for the Engine scripting system.

#include "entity_pool_bindings.h"

#include "engine/core/logging.h"

#include "entity_handle.h"
#include "reload_transaction.h"
#include "runtime_binding.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace engine::scripting {
namespace {

// The pools themselves live in the runtime bridge; scripting addresses
// them by slot and owns the Lua-visible ids.
std::size_t g_entityPoolCount = 0U;
/// Set by the first refusal of a full table and cleared when the pools
/// are reclaimed, so a script retrying every frame logs one line.
bool g_poolsFullReported = false;

/// Answers a refused pool_create with nil and `message`, which a script
/// can read to tell a full table from a bad argument.
int refuse_pool_create(lua_State *state, const char *message) noexcept {
  lua_pushnil(state);
  lua_pushstring(state, message);
  return 2;
}

// Lua-visible pool id layout: slot index in the low bits, the
// creating world's content epoch above it, mirroring the entity-handle
// scheme in entity_handle.cpp. reset_entity_pool_bindings() runs on
// every scene transition (engine_pipeline's process_pending_scene_op) to
// reclaim pool slots; the epoch field is what keeps that reclaim safe — a
// poolId a script held across the
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
  if ((outId == nullptr) || !runtime_bound()) {
    return false;
  }
  const auto epoch =
      static_cast<std::uint64_t>(runtime_binding().services->content_epoch(
          runtime_binding().world));
  const std::uint64_t encoded =
      (epoch << kPoolEpochShift) | (static_cast<std::uint64_t>(slot) + 1ULL);
  *outId = static_cast<lua_Integer>(encoded);
  return true;
}

/// Decodes a Lua pool id into a live slot index; false when malformed, out
/// of the currently allocated range, or stamped with a stale world epoch.
bool decode_pool_id(lua_Integer rawId, std::size_t *outSlot) noexcept {
  if ((outSlot == nullptr) || (rawId <= 0) ||
      !runtime_bound()) {
    return false;
  }
  const auto encoded = static_cast<std::uint64_t>(rawId);
  const std::uint64_t encodedSlot = encoded & kPoolSlotMask;
  const std::uint64_t encodedEpoch = encoded >> kPoolEpochShift;
  const auto currentEpoch =
      static_cast<std::uint64_t>(runtime_binding().services->content_epoch(
          runtime_binding().world));
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

/// Creates a fixed-size runtime entity pool from Lua. Returns the pool's
/// id, or nil and a reason: a bad count, a reload in progress, a World
/// with no room, or a full table, whose first refusal logs a Warning.
int lua_engine_pool_create(lua_State *state) noexcept {
  if (!runtime_bound()) {
    return refuse_pool_create(state, "pool_create has no world");
  }
  if (!lua_isinteger(state, 1)) {
    return refuse_pool_create(state, "pool_create expects an integer count");
  }
  // A pool seeds entities the reload scope cannot take back.
  if (reload_refuses("pool_create")) {
    return refuse_pool_create(state, "pool_create is refused during reload");
  }

  const lua_Integer count = lua_tointeger(state, 1);
  if ((count <= 0) ||
      (static_cast<std::size_t>(count) > kMaxEntityPoolSize)) {
    char message[96] = {};
    std::snprintf(message, sizeof(message),
                  "pool_create count must be 1 to %zu", kMaxEntityPoolSize);
    return refuse_pool_create(state, message);
  }

  if (g_entityPoolCount >= kMaxEntityPools) {
    char message[96] = {};
    std::snprintf(message, sizeof(message), "pool table full (%zu pools)",
                  kMaxEntityPools);
    if (!g_poolsFullReported) {
      g_poolsFullReported = true;
      core::log_message(core::LogLevel::Warning, "scripting", message);
    }
    return refuse_pool_create(state, message);
  }

  // The id is encoded before the pool is seeded so a slot never holds a
  // pool Lua cannot address.
  lua_Integer poolId = 0;
  if (!encode_pool_id(g_entityPoolCount, &poolId) ||
      !runtime_binding().services->entity_pool_init(
          runtime_binding().world, g_entityPoolCount,
          static_cast<std::size_t>(count))) {
    char message[96] = {};
    std::snprintf(message, sizeof(message),
                  "pool_create could not create %lld entities",
                  static_cast<long long>(count));
    core::log_message(core::LogLevel::Warning, "scripting", message);
    return refuse_pool_create(state, message);
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
  if (!decode_pool_id(lua_tointeger(state, 1), &slot) ||
      (reload_staging(ReloadEffect::PoolAcquire) == ReloadStaging::Refused)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity entity = runtime_binding().services->entity_pool_acquire(
      runtime_binding().world, slot);
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }
  reload_note_pool_acquire(slot, entity);

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

  bool ok = false;
  switch (reload_staging(ReloadEffect::PoolRelease)) {
  case ReloadStaging::None:
    ok = runtime_binding().services->entity_pool_release(
        runtime_binding().world, slot, entity);
    break;
  case ReloadStaging::Staged:
    reload_hold_pool_release(slot, entity);
    ok = true;
    break;
  case ReloadStaging::Refused:
    break;
  }
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
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->entity_pool_reset_all != nullptr)) {
    runtime_binding().services->entity_pool_reset_all();
  }
  g_entityPoolCount = 0U;
  g_poolsFullReported = false;
}

std::size_t pool_slot_count() noexcept { return g_entityPoolCount; }

} // namespace engine::scripting
