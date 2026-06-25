// Implements Lua entity-handle helpers for the Engine scripting system.

#include "entity_handle.h"

#include "runtime_binding.h"

#include <limits>

namespace engine::scripting {
namespace {

constexpr std::uint64_t kLuaEntityIndexMask = 0xFFFFFFFFULL;
constexpr unsigned kLuaEntityGenerationShift = 32U;

} // namespace

bool encode_lua_entity_handle(runtime::Entity entity,
                              lua_Integer *outHandle) noexcept {
  if ((outHandle == nullptr) || (entity.index == 0U) ||
      (entity.index >
       static_cast<std::uint32_t>(runtime::World::kMaxEntities)) ||
      (entity.generation == 0U)) {
    return false;
  }

  const std::uint64_t encodedGeneration =
      static_cast<std::uint64_t>(entity.generation - 1U);
  const std::uint64_t rawHandle =
      (encodedGeneration << kLuaEntityGenerationShift) |
      static_cast<std::uint64_t>(entity.index);
  if ((rawHandle == 0ULL) ||
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
  if ((outEntity == nullptr) || (rawHandle == 0ULL)) {
    return false;
  }

  const std::uint32_t entityIndex =
      static_cast<std::uint32_t>(rawHandle & kLuaEntityIndexMask);
  const std::uint64_t encodedGeneration =
      rawHandle >> kLuaEntityGenerationShift;
  if ((entityIndex == 0U) ||
      (entityIndex >
       static_cast<std::uint32_t>(runtime::World::kMaxEntities)) ||
      (encodedGeneration >
       static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max() -
                                  1U))) {
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
