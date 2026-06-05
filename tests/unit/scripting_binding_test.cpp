// Verifies scripting runtime binding ownership for the Engine test suite.

#include <cstdio>
#include <memory>
#include <new>

#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"

namespace {

int g_failed = 0;

/// Handles check.
void check(bool condition, const char *name) noexcept {
  if (!condition) {
    ++g_failed;
    std::fprintf(stderr, "FAIL: %s\n", name);
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  auto &globalLoc = engine::core::global_service_locator();
  globalLoc.clear();

  engine::core::ServiceLocator localLoc{};
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  engine::scripting::RuntimeServices services{};
  check(world != nullptr, "world allocated");
  if (world == nullptr) {
    return 1;
  }

  engine::scripting::bind_runtime_world(world.get(), localLoc);
  engine::scripting::bind_runtime_services(&services, localLoc);
  check(localLoc.get_service<engine::runtime::World>() == world.get(),
        "local world registered");
  check(localLoc.get_service<engine::scripting::RuntimeServices>() ==
            &services,
        "local services registered");
  check(globalLoc.count() == 0U, "explicit bind did not touch global");

  engine::scripting::bind_runtime_world(nullptr);
  engine::scripting::bind_runtime_services(nullptr);
  check(localLoc.get_service<engine::runtime::World>() == nullptr,
        "default null bind clears active local world");
  check(localLoc.get_service<engine::scripting::RuntimeServices>() == nullptr,
        "default null bind clears active local services");

  engine::scripting::bind_runtime_world(world.get());
  engine::scripting::bind_runtime_services(&services);
  check(globalLoc.get_service<engine::runtime::World>() == world.get(),
        "legacy world registered");
  check(globalLoc.get_service<engine::scripting::RuntimeServices>() ==
            &services,
        "legacy services registered");

  engine::scripting::bind_runtime_world(nullptr);
  engine::scripting::bind_runtime_services(nullptr);
  check(globalLoc.get_service<engine::runtime::World>() == nullptr,
        "legacy world removed");
  check(globalLoc.get_service<engine::scripting::RuntimeServices>() == nullptr,
        "legacy services removed");

  globalLoc.clear();
  return g_failed == 0 ? 0 : 1;
}
