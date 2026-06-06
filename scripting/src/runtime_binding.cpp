// Owns runtime binding state and service-locator registration for scripting.

#include "runtime_binding.h"

#include "engine/core/service_locator.h"

namespace engine::scripting {
namespace {

/// Returns the storage backing the scripting runtime binding.
ScriptingRuntimeBinding &binding_storage() noexcept {
  static ScriptingRuntimeBinding binding{};
  return binding;
}

} // namespace

/// Returns the process-local runtime binding state for scripting internals.
ScriptingRuntimeBinding &runtime_binding() noexcept { return binding_storage(); }

/// Binds scripting to a runtime world and mirrors the binding in services.
void bind_runtime_world(runtime::World *world) noexcept {
  bind_runtime_world(world, core::global_service_locator());
}

/// Binds scripting to a runtime world and mirrors it in an explicit locator.
void bind_runtime_world(runtime::World *world,
                        core::ServiceLocator &locator) noexcept {
  ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.worldLocator != nullptr) && (binding.worldLocator != &locator)) {
    static_cast<void>(binding.worldLocator->remove_service<runtime::World>());
  }

  binding.world = world;
  if (world != nullptr) {
    locator.register_service<runtime::World>(world);
    binding.worldLocator = &locator;
  } else {
    core::ServiceLocator *target =
        (binding.worldLocator != nullptr) ? binding.worldLocator : &locator;
    static_cast<void>(target->remove_service<runtime::World>());
    binding.worldLocator = nullptr;
  }
}

/// Binds scripting runtime callbacks and unregisters them on null.
void bind_runtime_services(const RuntimeServices *services) noexcept {
  bind_runtime_services(services, core::global_service_locator());
}

/// Binds scripting runtime callbacks and unregisters them on null.
void bind_runtime_services(const RuntimeServices *services,
                           core::ServiceLocator &locator) noexcept {
  ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.servicesLocator != nullptr) &&
      (binding.servicesLocator != &locator)) {
    static_cast<void>(
        binding.servicesLocator->remove_service<RuntimeServices>());
  }

  binding.services = services;
  if (services != nullptr) {
    locator.register_service<RuntimeServices>(
        const_cast<RuntimeServices *>(services));
    binding.servicesLocator = &locator;
  } else {
    core::ServiceLocator *target =
        (binding.servicesLocator != nullptr) ? binding.servicesLocator : &locator;
    static_cast<void>(target->remove_service<RuntimeServices>());
    binding.servicesLocator = nullptr;
  }
}

} // namespace engine::scripting
