// Declares private DAP APIs used by Lua debug hook integration.

#pragma once

#include "engine/scripting/dap_server.h"

#include <cstdint>

struct lua_State;

namespace engine::scripting {

/// Enumerates DAP step mode values used by the debug hook.
enum class DapStepMode : std::uint8_t {
  Continue,
  Next,
  StepIn,
  StepOut,
};

// Called from the Lua debug hook when execution should be paused.
// Blocks until the client sends a continue/step command.
// Returns the step mode to use after resuming.
DapStepMode dap_on_stopped(lua_State *L, const char *source, int line,
                           const char *reason) noexcept;

} // namespace engine::scripting
