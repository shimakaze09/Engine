// Implements the console commands the scripting system registers: spawn,
// which instantiates a prefab through the runtime bridge, from a path kept
// to the same VFS jail as every script path.

#include "console_commands.h"

#include "binding_util.h"
#include "runtime_binding.h"

#include <cstdio>

#include "engine/core/console.h"
#include "engine/core/text_parse.h"

namespace engine::scripting {
namespace {

/// Spawns a prefab from the console, at x y z when given. Only the
/// position is placed: the prefab keeps its authored rotation and scale,
/// as Unity's Instantiate(prefab, position, rotation) and a Godot
/// instance moved after instantiate() keep theirs. Coordinates that are
/// not three finite numbers refuse the command before anything spawns.
void cmd_spawn(const char *const *args, int argCount,
               void * /*userData*/) noexcept {
  if ((argCount != 2) && (argCount != 5)) {
    core::console_print("Usage: spawn <prefab> [x y z]");
    return;
  }
  math::Vec3 position{};
  if ((argCount == 5) && (!core::parse_float_token(args[2], &position.x) ||
                          !core::parse_float_token(args[3], &position.y) ||
                          !core::parse_float_token(args[4], &position.z))) {
    core::console_print("Spawn refused: x, y and z must be finite numbers");
    return;
  }
  if (!script_path_in_jail(args[1], "spawn")) {
    core::console_print("Spawn refused: the prefab path must stay inside the "
                        "project (relative, no '..')");
    return;
  }
  const RuntimeServices *const services = runtime_binding().services;
  if (!runtime_bound() || (services->instantiate_prefab == nullptr) ||
      (services->get_transform_op == nullptr) ||
      (services->add_transform_op == nullptr) ||
      (services->destroy_entity_op == nullptr)) {
    core::console_print("Cannot spawn: world not ready");
    return;
  }
  runtime::World *const world = runtime_binding().world;
  const runtime::Entity spawned = services->instantiate_prefab(world, args[1]);
  if (spawned == runtime::kInvalidEntity) {
    core::console_print("Spawn failed (prefab not found?)");
    return;
  }
  if (argCount == 5) {
    runtime::Transform transform{};
    if (!services->get_transform_op(world, spawned, &transform)) {
      transform = runtime::Transform{};
    }
    transform.position = position;
    if (!services->add_transform_op(world, spawned, transform)) {
      static_cast<void>(services->destroy_entity_op(world, spawned));
      core::console_print("Spawn failed: the instance could not be placed");
      return;
    }
  }
  char buffer[64] = {};
  std::snprintf(buffer, sizeof(buffer), "Spawned entity %u", spawned.index);
  core::console_print(buffer);
}

} // namespace

void register_console_commands() noexcept {
  // Changes the World outside any editor command, so an editor runs it in
  // Play only.
  core::console_register_world_command("spawn", cmd_spawn, nullptr,
                                       "Spawn a prefab: spawn <path> [x y z]");
}

} // namespace engine::scripting
