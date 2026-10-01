// Implements Lua entity-handle helpers for the Engine scripting system.

#include "entity_handle.h"

#include "runtime_binding.h"

#include <cstdio>
#include <limits>

#include "engine/core/hash.h"
#include "engine/core/logging.h"

extern "C" {
#include "lauxlib.h"
}

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

namespace {

/// Script lines that reported an entity argument this run. Past the table
/// one more line says the rest go unlisted, so a script failing on many
/// lines never logs every frame.
constexpr std::size_t kMaxReportedSites = 256U;
std::uint64_t g_reportedSites[kMaxReportedSites]{};
std::size_t g_reportedSiteCount = 0U;

/// Why a Lua value is not a live entity.
const char *entity_argument_problem(lua_State *state, int index) noexcept {
  if (!lua_isinteger(state, index) || (lua_tointeger(state, index) <= 0)) {
    return "is not an entity handle";
  }
  const auto raw = static_cast<std::uint64_t>(lua_tointeger(state, index));
  const std::uint32_t entityIndex =
      static_cast<std::uint32_t>(raw & kLuaEntityIndexMask);
  if ((entityIndex == 0U) ||
      (entityIndex > static_cast<std::uint32_t>(kMaxWorldEntities))) {
    return "is not an entity handle";
  }
  if (((raw >> kLuaEntityEpochShift) & kLuaEntityEpochMask) !=
      bound_world_epoch()) {
    return "is a handle from before the last scene load";
  }
  return "names an entity that was destroyed";
}

/// Logs a refused entity argument once per calling script line. The
/// location and binding name come from lua_getinfo, which reads them in
/// place, so the report allocates nothing on the Lua heap.
void report_entity_argument(lua_State *state, int index) noexcept {
  lua_Debug binding{};
  lua_Debug caller{};
  const bool hasBinding = (lua_getstack(state, 0, &binding) != 0) &&
                          (lua_getinfo(state, "n", &binding) != 0);
  const bool hasCaller = (lua_getstack(state, 1, &caller) != 0) &&
                         (lua_getinfo(state, "Sl", &caller) != 0);
  const char *const name =
      (hasBinding && (binding.name != nullptr)) ? binding.name : "?";
  const char *const source = hasCaller ? caller.short_src : "?";
  const int line = hasCaller ? caller.currentline : 0;

  std::uint64_t site = core::fnv1a_64(source);
  site = core::fnv1a_64_append_u64(site, core::fnv1a_64(name));
  site = core::fnv1a_64_append_u64(
      site, static_cast<std::uint64_t>(static_cast<std::uint32_t>(line)));
  const std::size_t recorded = (g_reportedSiteCount < kMaxReportedSites)
                                   ? g_reportedSiteCount
                                   : kMaxReportedSites;
  for (std::size_t i = 0U; i < recorded; ++i) {
    if (g_reportedSites[i] == site) {
      return;
    }
  }
  if (g_reportedSiteCount > kMaxReportedSites) {
    return;
  }
  char message[384] = {};
  std::snprintf(message, sizeof(message),
                "%s:%d: engine.%s: argument %d %s; the call does nothing%s",
                source, line, name, index,
                entity_argument_problem(state, index),
                (g_reportedSiteCount == kMaxReportedSites)
                    ? " (further lines with bad entity arguments are not "
                      "listed)"
                    : "");
  if (g_reportedSiteCount < kMaxReportedSites) {
    g_reportedSites[g_reportedSiteCount] = site;
  }
  ++g_reportedSiteCount;
  core::log_message(core::LogLevel::Warning, "scripting", message);
}

} // namespace

bool read_entity(lua_State *state, int index,
                 core::Entity *outEntity) noexcept {
  if (!runtime_bound() || (outEntity == nullptr)) {
    return false;
  }

  core::Entity decoded{};
  if (!decode_lua_entity_handle(state, index, &decoded) ||
      !runtime_binding().services->is_alive(runtime_binding().world, decoded)) {
    report_entity_argument(state, index);
    return false;
  }

  *outEntity = decoded;
  return true;
}

void reset_entity_argument_reports() noexcept { g_reportedSiteCount = 0U; }

} // namespace engine::scripting
