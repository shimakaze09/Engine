// Declares DAP server control APIs for the Engine scripting system.

#pragma once

#include <cstdint>

namespace engine::scripting {

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

} // namespace engine::scripting
