// Pins the bridge's handle contract through the production table: every
// RuntimeServices operation that names an entity takes the full handle,
// so after the World recycles an index the stale handle is refused by the
// op itself and the index's new occupant is never touched, while the live
// handle keeps working.

#include <cstdio>
#include <memory>
#include <new>

#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/runtime_services.h"
#include "engine/scripting/scripting.h"

namespace {

namespace rt = engine::runtime;
namespace sc = engine::scripting;

int g_failures = 0;

void check(bool condition, const char *name) noexcept {
  if (!condition) {
    std::printf("FAIL: %s\n", name);
    ++g_failures;
  }
}

/// Destroys `first` and creates the entity that takes over its index; the
/// caller asserts the recycle so a stale-handle refusal is never vacuous.
rt::Entity recycle_index(rt::World *world, rt::Entity first) noexcept {
  if (!world->destroy_entity(first)) {
    return rt::kInvalidEntity;
  }
  return world->create_scene_object();
}

void run(rt::World *world, const sc::RuntimeServices *services) noexcept {
  const rt::Entity first = world->create_scene_object();
  check(first != rt::kInvalidEntity, "create first entity");
  rt::Transform transform{};
  transform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  check(services->add_transform_op(world, first, transform),
        "live handle adds a transform");
  check(services->is_alive(world, first), "live handle is alive");

  const rt::Entity second = recycle_index(world, first);
  check(second != rt::kInvalidEntity, "create second entity");
  check(second.index == first.index,
        "second entity recycles the first entity's index");
  check(second.generation != first.generation,
        "second entity carries a new generation");

  // Identity and reads.
  check(!services->is_alive(world, first), "stale handle is not alive");
  check(services->persistent_id(world, first) == rt::kInvalidPersistentId,
        "stale handle has no persistent id");
  rt::Transform read{};
  check(!services->get_transform_op(world, first, &read),
        "stale handle reads no transform");
  check(services->get_transform_read_ptr(world, first) == nullptr,
        "stale handle reads no transform pointer");
  rt::RigidBody body{};
  check(!services->get_rigid_body_op(world, first, &body),
        "stale handle reads no rigid body");
  rt::ScriptComponent script{};
  check(!services->get_script_component_op(world, first, &script),
        "stale handle reads no script component");
  engine::math::CameraComponent camera{};
  check(!services->get_camera_component_op(world, first, &camera),
        "stale handle reads no camera component");
  check(!services->has_begun_play(world, first),
        "stale handle has not begun play");

  // Mutations: none reaches the occupant.
  transform.position = engine::math::Vec3(9.0F, 9.0F, 9.0F);
  check(!services->add_transform_op(world, first, transform),
        "stale handle cannot add a transform");
  body.inverseMass = 2.0F;
  check(!services->add_rigid_body_op(world, first, body),
        "stale handle cannot add a rigid body");
  rt::Collider collider{};
  check(!services->add_collider_op(world, first, collider),
        "stale handle cannot add a collider");
  rt::MeshComponent mesh{};
  check(!services->add_mesh_component_op(world, first, mesh),
        "stale handle cannot add a mesh component");
  rt::NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "%s", "ghost");
  check(!services->add_name_component_op(world, first, name),
        "stale handle cannot add a name");
  std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s",
                "assets/scripts/player.lua");
  check(!services->add_script_component_op(world, first, script),
        "stale handle cannot add a script component");
  check(!services->remove_script_component_op(world, first),
        "stale handle cannot remove a script component");
  check(!services->add_camera_component_op(world, first, camera),
        "stale handle cannot add a camera component");
  check(!services->remove_camera_component_op(world, first),
        "stale handle cannot remove a camera component");
  engine::math::SpringArmComponent arm{};
  check(!services->add_spring_arm_op(world, first, arm),
        "stale handle cannot add a spring arm");
  check(!services->set_movement_authority_op(world, first,
                                             rt::MovementAuthority::Script),
        "stale handle cannot take movement authority");
  check(!services->push_camera_op(world, first, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                                  1.0F, 1.0F, 5.0F),
        "stale handle cannot push a camera");
  check(!services->pop_camera_op(world, first),
        "stale handle cannot pop a camera");
  check(!services->is_sleeping(world, first),
        "stale handle is not a sleeping body");
  services->wake_body(world, first);
  check(services->clone_entity_op(world, first) == rt::kInvalidEntity,
        "stale handle cannot be cloned");
  check(services->add_fixed_joint(world, first, second) == 0U,
        "stale handle cannot anchor a joint");
  check(services->add_distance_joint(world, second, first, 1.0F) == 0U,
        "stale handle cannot be a joint endpoint");
  check(!services->destroy_entity_op(world, first),
        "stale handle cannot destroy the occupant");
  check(world->is_alive(second), "occupant survives the stale destroy");

  // The occupant is untouched by everything above: a scene object always
  // carries a transform, so the stale write is judged by its value.
  rt::Transform occupant{};
  check(world->get_transform(second, &occupant) &&
            (occupant.position.x != 9.0F),
        "occupant's transform is not the stale handle's write");
  check(!world->get_rigid_body(second, &body),
        "occupant gained no rigid body from the stale handle");
  check(!world->get_name_component(second, &name),
        "occupant gained no name from the stale handle");
  check(!world->get_script_component(second, &script),
        "occupant gained no script from the stale handle");

  // The live handle does the same work.
  check(services->add_transform_op(world, second, transform),
        "live handle adds a transform");
  check(services->get_transform_op(world, second, &read) &&
            (read.position.x == 9.0F),
        "live handle reads its transform");
  check(services->add_name_component_op(world, second, name),
        "live handle adds a name");
  check(services->persistent_id(world, second) != rt::kInvalidPersistentId,
        "live handle has a persistent id");
  const rt::Entity clone = services->clone_entity_op(world, second);
  check(clone != rt::kInvalidEntity, "live handle clones");
  check(services->destroy_entity_op(world, second),
        "live handle destroys its entity");
  check(!services->destroy_entity_op(nullptr, clone),
        "null world is refused");
}

} // namespace

int main() {
  if (!sc::initialize_scripting()) {
    std::printf("FAIL: initialize_scripting\n");
    return 1;
  }
  std::unique_ptr<rt::World> world(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator locator{};
  rt::bind_scripting_runtime(world.get(), locator);
  const auto *services = locator.get_service<sc::RuntimeServices>();
  if (services == nullptr) {
    std::printf("FAIL: the runtime bound no services table\n");
    rt::unbind_scripting_runtime(locator);
    sc::shutdown_scripting();
    return 1;
  }
  run(world.get(), services);
  rt::unbind_scripting_runtime(locator);
  sc::shutdown_scripting();
  if (g_failures != 0) {
    std::printf("script bridge handle tests: %d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
