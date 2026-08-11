// Verifies deferred Lua setter composition (#105): outside the Input phase
// each setter queues a whole-component snapshot, and before the fix a later
// setter read the committed state, so its snapshot clobbered fields written
// by an earlier same-frame setter. Drives real script-component BeginPlay
// dispatch and asserts every authored value survives the flush, including
// the bundled player scripts' restitution-then-friction pattern.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;
namespace math = engine::math;

constexpr const char *kModulePath = "deferred_write_module.lua";

/// Writes contents to a relative path.
bool write_file_at(const char *path, const char *contents) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0 || file == nullptr) {
    return false;
  }
#else
  file = std::fopen(path, "wb");
  if (file == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

/// Runs one BeginPlay phase in the pipeline's fixed order, flushing after
/// the phase ends exactly as stage_play_transitions does.
void run_begin_play_phase(rt::World *world) noexcept {
  world->begin_begin_play_phase();
  sc::dispatch_entity_scripts_begin_play(world);
  world->end_begin_play_phase();
  sc::flush_deferred_mutations();
}

/// Creates an entity carrying the given script path.
rt::Entity make_scripted_entity(rt::World *world, const char *path) noexcept {
  const rt::Entity entity = world->create_scene_object();
  rt::ScriptComponent scriptComponent{};
  std::snprintf(scriptComponent.scriptPath, sizeof(scriptComponent.scriptPath),
                "%s", path);
  if (!world->add_script_component(entity, scriptComponent)) {
    return rt::kInvalidEntity;
  }
  return entity;
}

// on_begin_play issues sequential same-component setters for the collider
// (the shipped player.lua pattern), rigid body, transform, mesh, and light,
// then pins the queued-destroy and queued-remove read-through boundaries.
constexpr const char *kModule =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    if not engine.set_restitution(self, 0.05) then\n"
    "        error('set_restitution failed')\n"
    "    end\n"
    "    if not engine.set_friction(self, 0.9, 0.7) then\n"
    "        error('set_friction failed')\n"
    "    end\n"
    "    if not engine.set_inverse_mass(self, 0.25) then\n"
    "        error('set_inverse_mass failed')\n"
    "    end\n"
    "    if not engine.set_velocity(self, 1.0, 2.0, 3.0) then\n"
    "        error('set_velocity failed')\n"
    "    end\n"
    "    if not engine.set_position(self, 4.0, 5.0, 6.0) then\n"
    "        error('set_position failed')\n"
    "    end\n"
    "    if not engine.set_scale(self, 2.0, 2.0, 2.0) then\n"
    "        error('set_scale failed')\n"
    "    end\n"
    "    if not engine.set_roughness(self, 0.25) then\n"
    "        error('set_roughness failed')\n"
    "    end\n"
    "    if not engine.set_metallic(self, 0.75) then\n"
    "        error('set_metallic failed')\n"
    "    end\n"
    "    if not engine.set_light_color(self, 1.0, 0.5, 0.25) then\n"
    "        error('set_light_color failed')\n"
    "    end\n"
    "    if not engine.set_light_intensity(self, 3.0) then\n"
    "        error('set_light_intensity failed')\n"
    "    end\n"
    "    -- Queued removal/destruction must read as absent: later setters\n"
    "    -- report failure instead of resurrecting from a stale snapshot.\n"
    "    if not engine.remove_light(self) then\n"
    "        error('remove_light failed')\n"
    "    end\n"
    "    if engine.set_light_intensity(self, 9.0) then\n"
    "        error('setter after queued light removal must fail')\n"
    "    end\n"
    "    local doomed = engine.find_entity_by_name('Doomed')\n"
    "    if doomed == nil then error('doomed lookup failed') end\n"
    "    if not engine.destroy_entity(doomed) then\n"
    "        error('queueing doomed destroy failed')\n"
    "    end\n"
    "    if engine.set_restitution(doomed, 0.9) then\n"
    "        error('setter after queued destroy must fail')\n"
    "    end\n"
    "    bp_completed = true\n"
    "end\n"
    "function verify_begin_play_completed()\n"
    "    if bp_completed ~= true then\n"
    "        error('on_begin_play did not run to completion')\n"
    "    end\n"
    "end\n"
    "return M\n";

} // namespace

/// Runs this executable or test program.
int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }

  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;
  ctx.check(write_file_at(kModulePath, kModule), "write script module");

  const rt::Entity entity = make_scripted_entity(world.get(), kModulePath);
  ctx.check(entity != rt::kInvalidEntity, "create scripted entity");

  // Distinct non-default component values so any snapshot rollback shows.
  rt::Collider collider{};
  collider.restitution = 0.3F;
  collider.staticFriction = 0.5F;
  collider.dynamicFriction = 0.3F;
  ctx.check(world->add_collider(entity, collider), "add collider");
  rt::RigidBody rigidBody{};
  rigidBody.inverseMass = 1.0F;
  ctx.check(world->add_rigid_body(entity, rigidBody), "add rigid body");
  rt::MeshComponent mesh{};
  mesh.meshAssetId = 7U;
  ctx.check(world->add_mesh_component(entity, mesh), "add mesh component");
  rt::LightComponent light{};
  light.intensity = 1.0F;
  ctx.check(world->add_light_component(entity, light), "add light");

  // A named victim for the queued-destroy boundary, with a committed
  // collider so only the pending destroy can make the setter fail.
  const rt::Entity doomed = world->create_scene_object();
  ctx.check(doomed != rt::kInvalidEntity, "create doomed entity");
  rt::NameComponent doomedName{};
  std::snprintf(doomedName.name, sizeof(doomedName.name), "Doomed");
  ctx.check(world->add_name_component(doomed, doomedName), "name doomed");
  rt::Collider doomedCollider{};
  ctx.check(world->add_collider(doomed, doomedCollider),
            "add doomed collider");

  run_begin_play_phase(world.get());

  ctx.check(sc::call_script_function("verify_begin_play_completed"),
            "on_begin_play ran to completion");
  ctx.check(!world->is_alive(doomed), "queued destroy applied at flush");

  rt::Collider outCollider{};
  ctx.check(world->get_collider(entity, &outCollider), "read collider");
  ctx.check(outCollider.restitution == 0.05F,
            "restitution survives the later friction write");
  ctx.check((outCollider.staticFriction == 0.9F) &&
                (outCollider.dynamicFriction == 0.7F),
            "friction values applied");

  rt::RigidBody outBody{};
  ctx.check(world->get_rigid_body(entity, &outBody), "read rigid body");
  ctx.check(outBody.inverseMass == 0.25F,
            "inverse mass survives the later velocity write");
  ctx.check((outBody.velocity.x == 1.0F) && (outBody.velocity.y == 2.0F) &&
                (outBody.velocity.z == 3.0F),
            "velocity applied");

  rt::Transform outTransform{};
  ctx.check(world->get_transform(entity, &outTransform), "read transform");
  ctx.check((outTransform.position.x == 4.0F) &&
                (outTransform.position.y == 5.0F) &&
                (outTransform.position.z == 6.0F),
            "position survives the later scale write");
  ctx.check((outTransform.scale.x == 2.0F) && (outTransform.scale.y == 2.0F) &&
                (outTransform.scale.z == 2.0F),
            "scale applied");

  rt::MeshComponent outMesh{};
  ctx.check(world->get_mesh_component(entity, &outMesh), "read mesh");
  ctx.check(outMesh.roughness == 0.25F,
            "roughness survives the later metallic write");
  ctx.check(outMesh.metallic == 0.75F, "metallic applied");
  ctx.check(outMesh.meshAssetId == 7U, "mesh asset id untouched");

  // The queued removal must win: the light is gone, not resurrected by the
  // rejected post-removal setter.
  ctx.check(!world->has_light_component(entity),
            "queued light removal is not resurrected");

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  static_cast<void>(std::remove(kModulePath));
  return ctx.finish("script_deferred_write");
}
