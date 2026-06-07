// Declares private runtime binding state used by the scripting module.

#pragma once

#include "engine/runtime/scripting_bridge.h"

namespace engine::scripting {

/// Tracks runtime pointers mirrored into service locators for Lua callbacks.
struct ScriptingRuntimeBinding final {
  runtime::World *world = nullptr;
  const RuntimeServices *services = nullptr;
  core::ServiceLocator *worldLocator = nullptr;
  core::ServiceLocator *servicesLocator = nullptr;
};

/// Returns the process-local runtime binding state for scripting internals.
ScriptingRuntimeBinding &runtime_binding() noexcept;

/// Clears runtime binding pointers and any locator entries they registered.
void clear_runtime_binding() noexcept;

} // namespace engine::scripting
