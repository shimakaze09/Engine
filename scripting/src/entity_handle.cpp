// Implements Lua entity-handle helpers for the Engine scripting system.

#include "entity_handle.h"

#include "runtime_binding.h"

#include <limits>

namespace engine::scripting {
namespace {

// Handle layout inside a positive 63-bit Lua integer: entity index in the
// low bits, generation above it, and the bound world's content epoch on
// top so handles retained across a whole-world replacement are rejected
// even when index and generation collide (audit C-03).
constexpr unsigned kLuaEntityIndexBits = 20U;
constexpr unsigned kLuaEntityGenerationBits = 26U;
constexpr unsigned kLuaEntityEpochBits = 17U;
constexpr std::uint64_t kLuaEntityIndexMask =
    (1ULL << kLuaEntityIndexBits) - 1ULL;
constexpr std::uint64_t kLuaEntityGenerationMask =
    (1ULL << kLuaEntityGenerationBits) - 1ULL;
constexpr std::uint64_t kLuaEntityEpochMask =
    (1ULL << kLuaEntityEpochBits) - 1ULL;
constexpr unsigned kLuaEntityGenerationShift = kLuaEntityIndexBits;
constexpr unsigned kLuaEntityEpochShift =
    kLuaEntityIndexBits + kLuaEntityGenerationBits;
static_assert(kLuaEntityIndexBits + kLuaEntityGenerationBits +
                      kLuaEntityEpochBits ==
                  63U,
              "handle layout must fit a positive lua_Integer");
static_assert(static_cast<std::uint64_t>(runtime::World::kMaxEntities) <=
                  kLuaEntityIndexMask,
              "entity index field too small for the configured capacity");

/// Content epoch of the bound world, masked to the handle field width.
std::uint64_t bound_world_epoch() noexcept {
  const runtime::World *world = runtime_binding().world;
  return (world != nullptr)
             ? (static_cast<std::uint64_t>(world->content_epoch()) &
                kLuaEntityEpochMask)
             : 0ULL;
}

} // namespace

bool encode_entity_handle_value(runtime::Entity entity,
                                std::uint64_t *outHandle) noexcept {
  if ((outHandle == nullptr) || (entity.index == 0U) ||
      (entity.index >
       static_cast<std::uint32_t>(runtime::World::kMaxEntities)) ||
      (entity.generation == 0U)) {
    return false;
  }

  const std::uint64_t encodedGeneration =
      static_cast<std::uint64_t>(entity.generation - 1U);
  if (encodedGeneration > kLuaEntityGenerationMask) {
    return false;
  }
  *outHandle = (bound_world_epoch() << kLuaEntityEpochShift) |
               (encodedGeneration << kLuaEntityGenerationShift) |
               static_cast<std::uint64_t>(entity.index);
  return *outHandle != 0ULL;
}

bool encode_lua_entity_handle(runtime::Entity entity,
                              lua_Integer *outHandle) noexcept {
  if (outHandle == nullptr) {
    return false;
  }

  std::uint64_t rawHandle = 0ULL;
  if (!encode_entity_handle_value(entity, &rawHandle) ||
      (rawHandle >
       static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max()))) {
    return false;
  }

  *outHandle = static_cast<lua_Integer>(rawHandle);
  return true;
}

void push_entity_handle(lua_State *state, runtime::Entity entity) noexcept {
  lua_Integer handle = 0;
  if (!encode_lua_entity_handle(entity, &handle)) {
    lua_pushnil(state);
    return;
  }

  lua_pushinteger(state, handle);
}

runtime::Entity entity_from_index(std::uint32_t entityIndex) noexcept {
  if (runtime_binding().world == nullptr) {
    return runtime::kInvalidEntity;
  }
  return runtime_binding().world->find_entity_by_index(entityIndex);
}

void push_entity_handle_from_index(lua_State *state,
                                   std::uint32_t entityIndex) noexcept {
  push_entity_handle(state, entity_from_index(entityIndex));
}

bool decode_entity_handle_value(std::uint64_t rawHandle,
                                runtime::Entity *outEntity) noexcept {
  if ((outEntity == nullptr) || (rawHandle == 0ULL) ||
      (runtime_binding().world == nullptr)) {
    return false;
  }

  const std::uint32_t entityIndex =
      static_cast<std::uint32_t>(rawHandle & kLuaEntityIndexMask);
  const std::uint64_t encodedGeneration =
      (rawHandle >> kLuaEntityGenerationShift) & kLuaEntityGenerationMask;
  const std::uint64_t encodedEpoch =
      (rawHandle >> kLuaEntityEpochShift) & kLuaEntityEpochMask;
  if ((entityIndex == 0U) ||
      (entityIndex >
       static_cast<std::uint32_t>(runtime::World::kMaxEntities)) ||
      (encodedEpoch != bound_world_epoch())) {
    return false;
  }

  *outEntity = runtime::Entity{
      entityIndex, static_cast<std::uint32_t>(encodedGeneration + 1ULL)};
  return true;
}

bool decode_lua_entity_handle(lua_State *state, int index,
                              runtime::Entity *outEntity) noexcept {
  if ((outEntity == nullptr) || !lua_isnumber(state, index)) {
    return false;
  }

  const lua_Integer rawHandleSigned = lua_tointeger(state, index);
  if (rawHandleSigned <= 0) {
    return false;
  }

  return decode_entity_handle_value(static_cast<std::uint64_t>(rawHandleSigned),
                                    outEntity);
}

bool read_entity(lua_State *state, int index,
                 runtime::Entity *outEntity) noexcept {
  if ((runtime_binding().world == nullptr) || (outEntity == nullptr)) {
    return false;
  }

  runtime::Entity decoded{};
  if (!decode_lua_entity_handle(state, index, &decoded) ||
      !runtime_binding().world->is_alive(decoded)) {
    return false;
  }

  *outEntity = decoded;
  return true;
}

} // namespace engine::scripting
