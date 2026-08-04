// Implements shared Lua binding helpers (argument parsing and error logging)
// for the scripting module's binding translation units.

#include "binding_util.h"

#include "debug_bindings.h"
#include "lua_state.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cmath>
#include <cstdio>

#include "engine/core/logging.h"

namespace engine::scripting {
namespace {

/// Protected traceback helper: receives the raw error message via light
/// userdata so an allocation failure inside luaL_traceback stays catchable
/// instead of reaching the panic handler.
int traceback_trampoline(lua_State *state) noexcept {
  const char *message = static_cast<const char *>(lua_touserdata(state, 1));
  luaL_traceback(state, state, message, 1);
  return 1;
}

/// Carries a chunk path into the protected load trampoline.
struct ChunkLoadArgs final {
  const char *path = nullptr;
};

/// Protected trampoline: loads the chunk file, re-raising the load error
/// (syntax, I/O, or memory) so the wrapping pcall catches it.
int chunk_load_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<ChunkLoadArgs *>(lua_touserdata(state, 1));
  if (luaL_loadfile(state, args->path) != LUA_OK) {
    return lua_error(state);
  }
  return 1;
}

/// Carries the created registry reference out of the ref trampoline.
struct RegistryRefArgs final {
  int ref = LUA_NOREF;
};

/// Protected trampoline: refs the value argument into the registry so a
/// registry-growth allocation failure stays catchable.
int registry_ref_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<RegistryRefArgs *>(lua_touserdata(state, 1));
  args->ref = luaL_ref(state, LUA_REGISTRYINDEX);
  return 0;
}

} // namespace

/// Reads vec3 args data. Non-finite components are rejected here so a NaN
/// or infinity from Lua fails at the call that produced it rather than
/// being logged by whichever World ingress it eventually reaches.
bool read_vec3_args(lua_State *state, int startIndex,
                    math::Vec3 *outVec) noexcept {
  if ((outVec == nullptr) || !lua_isnumber(state, startIndex) ||
      !lua_isnumber(state, startIndex + 1) ||
      !lua_isnumber(state, startIndex + 2)) {
    return false;
  }

  const float x = static_cast<float>(lua_tonumber(state, startIndex));
  const float y = static_cast<float>(lua_tonumber(state, startIndex + 1));
  const float z = static_cast<float>(lua_tonumber(state, startIndex + 2));
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return false;
  }
  *outVec = math::Vec3(x, y, z);
  return true;
}

/// Reads finite number args data: the scalar counterpart of
/// read_vec3_args, so scalar bindings reject non-finite input the same way.
bool read_finite_number_arg(lua_State *state, int index,
                            float *outValue) noexcept {
  if ((outValue == nullptr) || !lua_isnumber(state, index)) {
    return false;
  }
  const float value = static_cast<float>(lua_tonumber(state, index));
  if (!std::isfinite(value)) {
    return false;
  }
  *outValue = value;
  return true;
}

void log_lua_error(const char *context) noexcept {
  log_lua_error(current_lua_state(), context);
}

void log_lua_error(lua_State *state, const char *context) noexcept {
  if (state == nullptr) {
    return;
  }

  const char *message = lua_tostring(state, -1);
  if (message == nullptr) {
    message = "unknown lua error";
  }

  lua_pushcfunction(state, &traceback_trampoline);
  lua_pushlightuserdata(state,
                        const_cast<void *>(static_cast<const void *>(message)));
  const char *trace = message;
  if (lua_pcall(state, 1, 1, 0) == LUA_OK) {
    const char *result = lua_tostring(state, -1);
    if (result != nullptr) {
      trace = result;
    }
  }

  char logBuffer[1024] = {};
  if ((context != nullptr) && (context[0] != '\0')) {
    std::snprintf(logBuffer, sizeof(logBuffer), "lua error (%s): %s", context,
                  trace);
  } else {
    std::snprintf(logBuffer, sizeof(logBuffer), "lua error: %s", trace);
  }
  core::log_message(core::LogLevel::Error, "scripting", logBuffer);
  lua_pop(state, 2);
}

bool protected_c_operation(lua_State *state, LuaDispatchFn trampoline,
                           void *args, int nresults,
                           const char *context) noexcept {
  if ((state == nullptr) || (trampoline == nullptr)) {
    return false;
  }

  lua_pushcfunction(state, trampoline);
  lua_pushlightuserdata(state, args);
  if (lua_pcall(state, 1, nresults, 0) != LUA_OK) {
    log_lua_error(state, context);
    return false;
  }
  return true;
}

bool protected_load_chunk(lua_State *state, const char *path,
                          const char *context) noexcept {
  if ((state == nullptr) || (path == nullptr)) {
    return false;
  }
  ChunkLoadArgs args{};
  args.path = path;
  return protected_c_operation(state, &chunk_load_trampoline, &args, 1,
                               context);
}

bool protected_registry_ref(lua_State *state, int *outRef,
                            const char *context) noexcept {
  if (outRef != nullptr) {
    *outRef = LUA_NOREF;
  }
  if ((state == nullptr) || (outRef == nullptr) || (lua_gettop(state) < 1)) {
    return false;
  }

  RegistryRefArgs args{};
  lua_pushcfunction(state, &registry_ref_trampoline);
  lua_pushlightuserdata(state, &args);
  lua_rotate(state, -3, 2);
  if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
    log_lua_error(state, context);
    return false;
  }
  *outRef = args.ref;
  return true;
}

bool protected_engine_dispatch(lua_State *state, LuaDispatchFn trampoline,
                               void *args, int nresults,
                               const char *context) noexcept {
  if ((state == nullptr) || (trampoline == nullptr)) {
    return false;
  }

  arm_debug_lua_hook(state);
  if (!protected_c_operation(state, trampoline, args, nresults, context)) {
    return false;
  }

  if (debug_instruction_budget_exhausted()) {
    lua_pop(state, nresults);
    char logBuffer[256] = {};
    std::snprintf(logBuffer, sizeof(logBuffer),
                  "lua dispatch aborted (%s): CPU instruction budget exhausted",
                  (context != nullptr) ? context : "");
    core::log_message(core::LogLevel::Error, "scripting", logBuffer);
    return false;
  }

  return true;
}

} // namespace engine::scripting
