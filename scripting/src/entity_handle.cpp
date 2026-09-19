// Implements Lua entity-handle helpers for the Engine scripting system.

#include "entity_handle.h"

#include "runtime_binding.h"

#include <limits>

namespace engine::scripting {
namespace {

// Handle layout inside a positive 63-bit Lua integer: entity index in the
// low bits, generation above it, and the bound world's content epoch on
// top so handles retained across a whole-world replacement are rejected
// even when index and generation collide.
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
static_assert(static_cast<std::uint64_t>(kMaxWorldEntities) <=
                  kLuaEntityIndexMask,
              "entity index field too small for the configured capacity");

/// Content epoch of the bound world, masked to the handle field width; the
/// first time the raw epoch exceeds the field, the weakened stale-handle
/// guarantee is announced instead of masking silently.
std::uint64_t bound_world_epoch() noexcept {
  if (!runtime_bound()) {
    return 0ULL;
  }
  const std::uint32_t epoch =
      runtime_binding().services->content_epoch(runtime_binding().world);
  static bool warnedExhausted = false;
  if ((static_cast<std::uint64_t>(epoch) > kLuaEntityEpochMask) &&
      !warnedExhausted) {
    warnedExhausted = true;
    core::log_message(core::LogLevel::Warning, "scripting",
                      "entity-handle epoch field exhausted: handles retained "
                      "across 131072+ world replacements can alias");
  }
  return static_cast<std::uint64_t>(epoch) & kLuaEntityEpochMask;
}

} // namespace

bool encode_entity_handle_value(core::Entity entity,
                                std::uint64_t *outHandle) noexcept {
  if ((outHandle == nullptr) || (entity.index == 0U) ||
      (entity.index > static_cast<std::uint32_t>(kMaxWorldEntities)) ||
      (entity.generation == 0U)) {
    return false;
  }

  const std::uint64_t encodedGeneration =
      static_cast<std::uint64_t>(entity.generation - 1U);
  if (encodedGeneration > kLuaEntityGenerationMask) {
    return false;
  }
  if (!runtime_bound() ||
      !runtime_binding().services->is_alive(runtime_binding().world, entity)) {
    return false;
  }
  *outHandle = (bound_world_epoch() << kLuaEntityEpochShift) |
               (encodedGeneration << kLuaEntityGenerationShift) |
               static_cast<std::uint64_t>(entity.index);
  return *outHandle != 0ULL;
}

bool encode_lua_entity_handle(core::Entity entity,
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

void push_entity_handle(lua_State *state, core::Entity entity) noexcept {
  lua_Integer handle = 0;
  if (!encode_lua_entity_handle(entity, &handle)) {
    lua_pushnil(state);
    return;
  }

  lua_pushinteger(state, handle);
}

bool decode_entity_handle_value(std::uint64_t rawHandle,
                                core::Entity *outEntity) noexcept {
  if ((outEntity == nullptr) || (rawHandle == 0ULL) || !runtime_bound()) {
    return false;
  }

  const std::uint32_t entityIndex =
      static_cast<std::uint32_t>(rawHandle & kLuaEntityIndexMask);
  const std::uint64_t encodedGeneration =
      (rawHandle >> kLuaEntityGenerationShift) & kLuaEntityGenerationMask;
  const std::uint64_t encodedEpoch =
      (rawHandle >> kLuaEntityEpochShift) & kLuaEntityEpochMask;
  if ((entityIndex == 0U) ||
      (entityIndex > static_cast<std::uint32_t>(kMaxWorldEntities)) ||
      (encodedEpoch != bound_world_epoch())) {
    return false;
  }

  *outEntity = core::Entity{
      entityIndex, static_cast<std::uint32_t>(encodedGeneration + 1ULL)};
  return true;
}

bool decode_lua_entity_handle(lua_State *state, int index,
                              core::Entity *outEntity) noexcept {
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
                 core::Entity *outEntity) noexcept {
  if (!runtime_bound() || (outEntity == nullptr)) {
    return false;
  }

  core::Entity decoded{};
  if (!decode_lua_entity_handle(state, index, &decoded) ||
      !runtime_binding().services->is_alive(runtime_binding().world, decoded)) {
    return false;
  }

  *outEntity = decoded;
  return true;
}

} // namespace engine::scripting
