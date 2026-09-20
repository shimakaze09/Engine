// Link evidence for the scripting boundary: this executable links
// engine_scripting and its declared downward dependencies only, never the
// runtime, and still initialises the VM and registers every binding. Any
// scripting object that reached a runtime symbol would fail to link here.

#include <cstdio>

#include "engine/core/service_locator.h"
#include "engine/scripting/bindable_api.h"
#include "engine/scripting/runtime_services.h"
#include "engine/scripting/scripting.h"

int main() {
  if (!engine::scripting::initialize_scripting()) {
    std::printf("FAIL: initialize_scripting\n");
    return 1;
  }

  // An empty services table binds and unbinds like the runtime's; with no
  // World bound every world-facing query answers its unbound value.
  engine::core::ServiceLocator locator{};
  const engine::scripting::RuntimeServices services{};
  engine::scripting::bind_runtime_services(&services, locator);
  int failures = 0;
  if (engine::scripting::bindable_get_entity_count() != 0) {
    std::printf("FAIL: unbound entity count is not zero\n");
    ++failures;
  }
  if (engine::scripting::bindable_is_alive(1ULL)) {
    std::printf("FAIL: unbound world reports a live handle\n");
    ++failures;
  }
  engine::scripting::bind_runtime_services(nullptr, locator);
  engine::scripting::shutdown_scripting();
  return failures;
}
