// Implements the console commands the scripting system registers: spawn,
// which instantiates a prefab through the runtime bridge.

#include "console_commands.h"

#include "runtime_binding.h"

#include <cstdio>
#include <cstdlib>

#include "engine/core/console.h"

namespace engine::scripting {
namespace {

/// Spawns a prefab from the console.
void cmd_spawn(const char *const *args, int argCount,
               void * /*userData*/) noexcept {
  if (argCount < 2) {
    core::console_print("Usage: spawn <prefab> [x y z]");
    return;
  }
  if (!runtime_bound() ||
      (runtime_binding().services->instantiate_prefab == nullptr)) {
    core::console_print("Cannot spawn: world not ready");
    return;
  }
  const runtime::Entity spawned =
      runtime_binding().services->instantiate_prefab(runtime_binding().world,
                                                     args[1]);
  if (spawned == runtime::kInvalidEntity) {
    core::console_print("Spawn failed (prefab not found?)");
    return;
  }
  if ((argCount >= 5) &&
      (runtime_binding().services->add_transform_op != nullptr)) {
    runtime::Transform transform{};
    transform.position.x = static_cast<float>(std::atof(args[2]));
    transform.position.y = static_cast<float>(std::atof(args[3]));
    transform.position.z = static_cast<float>(std::atof(args[4]));
    transform.scale = {1.0F, 1.0F, 1.0F};
    transform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
    runtime_binding().services->add_transform_op(runtime_binding().world,
                                                 spawned, transform);
  }
  char buffer[64] = {};
  std::snprintf(buffer, sizeof(buffer), "Spawned entity %u", spawned.index);
  core::console_print(buffer);
}

} // namespace

void register_console_commands() noexcept {
  core::console_register_command("spawn", cmd_spawn, nullptr,
                                 "Spawn a prefab: spawn <path> [x y z]");
}

} // namespace engine::scripting
