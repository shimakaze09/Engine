// Declares dap server types and APIs for the Engine Lua scripting system.

#pragma once

#include <cstdint>

/// Stores lua state data used by the engine.
struct lua_State;

namespace engine::scripting {

/// Enumerates dap step mode values used by the engine.
enum class DapStepMode : std::uint8_t {
  Continue,
  Next,
  StepIn,
  StepOut,
};

// Start the DAP debugger server on the specified TCP port.
bool dap_start(std::uint16_t port) noexcept;

// Stop the DAP debugger server and close all connections.
void dap_stop() noexcept;

// Returns true if the DAP server is running and listening.
bool dap_is_running() noexcept;

// Returns true if a DAP client is connected.
bool dap_has_client() noexcept;

// Poll for new connections (non-blocking). Call once per frame.
void dap_poll() noexcept;

// Called from the Lua debug hook when execution should be paused.
// Blocks until the client sends a continue/step command.
// Returns the step mode to use after resuming.
DapStepMode dap_on_stopped(lua_State *L, const char *source, int line,
                           const char *reason) noexcept;

} // namespace engine::scripting
