// Declares private runtime binding state used by the scripting module.

#pragma once

#include "engine/scripting/runtime_services.h"

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

/// True while both a World and the services table are bound; every World
/// operation goes through the table, so one without the other is unbound.
inline bool runtime_bound() noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  return (binding.world != nullptr) && (binding.services != nullptr);
}

/// Clears runtime binding pointers and any locator entries they registered.
void clear_runtime_binding() noexcept;

} // namespace engine::scripting
