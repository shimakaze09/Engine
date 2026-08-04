// Verifies the scripting runtime binding's service-locator ownership rules
// (audit M-14): binds are last-writer-wins, but unbinding must remove a
// locator entry only while it still holds the pointer this bridge
// registered, so a newer provider registered by another owner survives.

#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <memory>
#include <new>

namespace {

using engine::core::ServiceLocator;
using engine::runtime::World;
using engine::scripting::RuntimeServices;

/// Unbinding after another owner replaced the World entry must keep the
/// replacement registered.
bool test_world_unbind_preserves_newer_provider(World *worldA,
                                                World *worldB) noexcept {
  ServiceLocator locator{};
  engine::scripting::bind_runtime_world(worldA, locator);
  if (locator.get_service<World>() != worldA) {
    return false;
  }

  static_cast<void>(locator.register_service<World>(worldB));

  engine::scripting::bind_runtime_world(nullptr, locator);
  return locator.get_service<World>() == worldB;
}

/// A plain bind/unbind cycle must still clean up its own entry.
bool test_world_unbind_removes_owned_entry(World *worldA) noexcept {
  ServiceLocator locator{};
  engine::scripting::bind_runtime_world(worldA, locator);
  engine::scripting::bind_runtime_world(nullptr, locator);
  return !locator.has_service<World>();
}

/// Rebinding into a different locator must not clobber a replacement that
/// took over the old locator's entry.
bool test_world_locator_switch_preserves_replacement(World *worldA,
                                                     World *worldB) noexcept {
  ServiceLocator locatorOld{};
  ServiceLocator locatorNew{};
  engine::scripting::bind_runtime_world(worldA, locatorOld);
  static_cast<void>(locatorOld.register_service<World>(worldB));

  engine::scripting::bind_runtime_world(worldA, locatorNew);
  const bool ok = (locatorOld.get_service<World>() == worldB) &&
                  (locatorNew.get_service<World>() == worldA);
  engine::scripting::bind_runtime_world(nullptr, locatorNew);
  return ok;
}

/// Same ownership contract for the RuntimeServices entry.
bool test_services_unbind_preserves_newer_provider() noexcept {
  static RuntimeServices servicesA{};
  static RuntimeServices servicesB{};

  ServiceLocator locator{};
  engine::scripting::bind_runtime_services(&servicesA, locator);
  if (locator.get_service<RuntimeServices>() != &servicesA) {
    return false;
  }

  static_cast<void>(locator.register_service<RuntimeServices>(&servicesB));

  engine::scripting::bind_runtime_services(nullptr, locator);
  if (locator.get_service<RuntimeServices>() != &servicesB) {
    return false;
  }

  static_cast<void>(locator.remove_service<RuntimeServices>());
  return true;
}

/// Unbinding when nothing was ever bound must not remove another owner's
/// entry.
bool test_unbind_without_bind_is_inert(World *worldB) noexcept {
  ServiceLocator locator{};
  static_cast<void>(locator.register_service<World>(worldB));
  engine::scripting::bind_runtime_world(nullptr, locator);
  const bool ok = locator.get_service<World>() == worldB;
  static_cast<void>(locator.remove_service<World>());
  return ok;
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::unique_ptr<World> worldA(new (std::nothrow) World());
  std::unique_ptr<World> worldB(new (std::nothrow) World());
  if ((worldA == nullptr) || (worldB == nullptr)) {
    std::fprintf(stderr, "world allocation failed\n");
    return 1;
  }

  if (!test_world_unbind_preserves_newer_provider(worldA.get(),
                                                  worldB.get())) {
    std::fprintf(stderr, "world unbind clobbered a newer provider\n");
    return 2;
  }
  if (!test_world_unbind_removes_owned_entry(worldA.get())) {
    std::fprintf(stderr, "world unbind left its own entry behind\n");
    return 3;
  }
  if (!test_world_locator_switch_preserves_replacement(worldA.get(),
                                                       worldB.get())) {
    std::fprintf(stderr, "locator switch clobbered a replacement\n");
    return 4;
  }
  if (!test_services_unbind_preserves_newer_provider()) {
    std::fprintf(stderr, "services unbind clobbered a newer provider\n");
    return 5;
  }
  if (!test_unbind_without_bind_is_inert(worldB.get())) {
    std::fprintf(stderr, "unbind without bind removed a foreign entry\n");
    return 6;
  }

  std::printf("runtime_binding_test passed\n");
  return 0;
}
