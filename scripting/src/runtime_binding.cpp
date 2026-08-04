// Owns runtime binding state and service-locator registration for scripting.
// Locator entries follow last-writer-wins on bind; unbind only removes an
// entry this binding still owns (the registered pointer is unchanged), so a
// newer provider registered by someone else is never clobbered (audit M-14).

#include "runtime_binding.h"

#include "engine/core/service_locator.h"

namespace engine::scripting {
namespace {

/// Returns the storage backing the scripting runtime binding.
ScriptingRuntimeBinding &binding_storage() noexcept {
  static ScriptingRuntimeBinding binding{};
  return binding;
}

/// Removes the locator's World entry only while it still holds `world`.
void remove_world_if_owned(core::ServiceLocator *locator,
                           runtime::World *world) noexcept {
  if ((locator != nullptr) && (world != nullptr) &&
      (locator->get_service<runtime::World>() == world)) {
    static_cast<void>(locator->remove_service<runtime::World>());
  }
}

/// Removes the locator's RuntimeServices entry only while it still holds
/// `services`.
void remove_services_if_owned(core::ServiceLocator *locator,
                              const RuntimeServices *services) noexcept {
  if ((locator != nullptr) && (services != nullptr) &&
      (locator->get_service<RuntimeServices>() == services)) {
    static_cast<void>(locator->remove_service<RuntimeServices>());
  }
}

} // namespace

/// Returns the process-local runtime binding state for scripting internals.
ScriptingRuntimeBinding &runtime_binding() noexcept { return binding_storage(); }

/// Binds scripting to a runtime world and mirrors it in an explicit locator.
void bind_runtime_world(runtime::World *world,
                        core::ServiceLocator &locator) noexcept {
  ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.worldLocator != nullptr) && (binding.worldLocator != &locator)) {
    remove_world_if_owned(binding.worldLocator, binding.world);
  }

  if (world != nullptr) {
    binding.world = world;
    locator.register_service<runtime::World>(world);
    binding.worldLocator = &locator;
  } else {
    core::ServiceLocator *target =
        (binding.worldLocator != nullptr) ? binding.worldLocator : &locator;
    remove_world_if_owned(target, binding.world);
    binding.world = nullptr;
    binding.worldLocator = nullptr;
  }
}

/// Binds scripting runtime callbacks and unregisters them on null.
void bind_runtime_services(const RuntimeServices *services,
                           core::ServiceLocator &locator) noexcept {
  ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.servicesLocator != nullptr) &&
      (binding.servicesLocator != &locator)) {
    remove_services_if_owned(binding.servicesLocator, binding.services);
  }

  if (services != nullptr) {
    binding.services = services;
    locator.register_service<RuntimeServices>(
        const_cast<RuntimeServices *>(services));
    binding.servicesLocator = &locator;
  } else {
    core::ServiceLocator *target =
        (binding.servicesLocator != nullptr) ? binding.servicesLocator : &locator;
    remove_services_if_owned(target, binding.services);
    binding.services = nullptr;
    binding.servicesLocator = nullptr;
  }
}

/// Clears runtime binding pointers and any locator entries this binding
/// still owns.
void clear_runtime_binding() noexcept {
  ScriptingRuntimeBinding &binding = runtime_binding();
  remove_world_if_owned(binding.worldLocator, binding.world);
  remove_services_if_owned(binding.servicesLocator, binding.services);
  binding.world = nullptr;
  binding.services = nullptr;
  binding.worldLocator = nullptr;
  binding.servicesLocator = nullptr;
}

} // namespace engine::scripting
