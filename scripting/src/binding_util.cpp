// Implements shared Lua binding helpers (argument parsing and error logging)
// for the scripting module's binding translation units.

#include "binding_util.h"

#include "lua_state.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstdio>

#include "engine/core/logging.h"

namespace engine::scripting {

/// Reads vec3 args data.
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
  *outVec = math::Vec3(x, y, z);
  return true;
}

void log_lua_error(const char *context) noexcept {
  lua_State *state = current_lua_state();
  if (state == nullptr) {
    return;
  }

  const char *message = lua_tostring(state, -1);
  if (message == nullptr) {
    message = "unknown lua error";
  }

  // Attach traceback so logs include script file and line diagnostics.
  luaL_traceback(state, state, message, 1);
  const char *trace = lua_tostring(state, -1);
  if (trace == nullptr) {
    trace = message;
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

} // namespace engine::scripting
