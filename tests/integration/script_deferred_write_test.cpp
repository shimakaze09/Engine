// Verifies deferred Lua setter composition (#105): outside the Input phase
// each setter queues a whole-component snapshot, and before the fix a later
// setter read the committed state, so its snapshot clobbered fields written
// by an earlier same-frame setter. Drives real script-component BeginPlay
// dispatch and asserts every authored value survives the flush, including
// the bundled player scripts' restitution-then-friction pattern.
//
// Also verifies the read side of the same contract (#125): pure getters
// route through the latest_* read-through helpers, so a same-frame
// get-after-set inside on_begin_play sees the just-queued write rather than
// stale committed state, a getter on an entity with a queued destroy reads
// as absent, and a getter with no pending write at all still reads the
// ordinary committed value.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/logging.h"
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
    "-- Getter return values round-trip through a C++ float; a Lua double\n"
    "-- literal for a non-power-of-two fraction (0.3, 0.05, 0.9, ...) is not\n"
    "-- bit-identical to that float widened to double, so comparisons use a\n"
    "-- tolerance justified by float32-to-double promotion error (<< 1e-5\n"
    "-- for these O(1)-magnitude values).\n"
    "local function near(a, b)\n"
    "    return math.abs(a - b) < 1e-5\n"
    "end\n"
    "function M.on_begin_play(self)\n"
    "    local anchor = engine.find_entity_by_name('Anchor')\n"
    "    if anchor == nil then error('anchor lookup failed') end\n"
    "    -- #125 boundary: no pending write yet, so the getter must still\n"
    "    -- read the ordinary committed value.\n"
    "    if not near(engine.get_restitution(self), 0.3) then\n"
    "        error('get_restitution before any write did not read committed '\n"
    "              .. 'state')\n"
    "    end\n"
    "    if not engine.set_restitution(self, 0.05) then\n"
    "        error('set_restitution failed')\n"
    "    end\n"
    "    -- #125 boundary: get-after-set in the same frame reads the queued\n"
    "    -- write, not the pre-write committed snapshot.\n"
    "    if not near(engine.get_restitution(self), 0.05) then\n"
    "        error('get_restitution after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_friction(self, 0.9, 0.7) then\n"
    "        error('set_friction failed')\n"
    "    end\n"
    "    local fs, fd = engine.get_friction(self)\n"
    "    if not (near(fs, 0.9) and near(fd, 0.7)) then\n"
    "        error('get_friction after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_inverse_mass(self, 0.25) then\n"
    "        error('set_inverse_mass failed')\n"
    "    end\n"
    "    if not near(engine.get_inverse_mass(self), 0.25) then\n"
    "        error('get_inverse_mass after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_velocity(self, 1.0, 2.0, 3.0) then\n"
    "        error('set_velocity failed')\n"
    "    end\n"
    "    local vx, vy, vz = engine.get_velocity(self)\n"
    "    if not (near(vx, 1.0) and near(vy, 2.0) and near(vz, 3.0)) then\n"
    "        error('get_velocity after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_angular_velocity(self, 0.1, 0.2, 0.3) then\n"
    "        error('set_angular_velocity failed')\n"
    "    end\n"
    "    local avx, avy, avz = engine.get_angular_velocity(self)\n"
    "    if not (near(avx, 0.1) and near(avy, 0.2) and near(avz, 0.3)) then\n"
    "        error('get_angular_velocity after set did not read the queued '\n"
    "              .. 'write')\n"
    "    end\n"
    "    if not engine.set_position(self, 4.0, 5.0, 6.0) then\n"
    "        error('set_position failed')\n"
    "    end\n"
    "    local px, py, pz = engine.get_position(self)\n"
    "    if not (near(px, 4.0) and near(py, 5.0) and near(pz, 6.0)) then\n"
    "        error('get_position after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_rotation(self, 0.0, 0.0, 0.0, 1.0) then\n"
    "        error('set_rotation failed')\n"
    "    end\n"
    "    local rx, ry, rz, rw = engine.get_rotation(self)\n"
    "    if not (near(rx, 0.0) and near(ry, 0.0) and near(rz, 0.0) and\n"
    "            near(rw, 1.0)) then\n"
    "        error('get_rotation after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_scale(self, 2.0, 2.0, 2.0) then\n"
    "        error('set_scale failed')\n"
    "    end\n"
    "    local sx, sy, sz = engine.get_scale(self)\n"
    "    if not (near(sx, 2.0) and near(sy, 2.0) and near(sz, 2.0)) then\n"
    "        error('get_scale after set did not read the queued write')\n"
    "    end\n"
    "    local probe = engine.find_entity_by_name('ParentProbe')\n"
    "    if probe == nil then error('parent probe lookup failed') end\n"
    "    if not engine.set_parent(probe, anchor) then\n"
    "        error('set_parent failed')\n"
    "    end\n"
    "    if engine.get_parent(probe) ~= anchor then\n"
    "        error('get_parent after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_half_extents(self, 1.5, 1.5, 1.5) then\n"
    "        error('set_half_extents failed')\n"
    "    end\n"
    "    local hx, hy, hz = engine.get_half_extents(self)\n"
    "    if not (near(hx, 1.5) and near(hy, 1.5) and near(hz, 1.5)) then\n"
    "        error('get_half_extents after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_roughness(self, 0.25) then\n"
    "        error('set_roughness failed')\n"
    "    end\n"
    "    if not near(engine.get_roughness(self), 0.25) then\n"
    "        error('get_roughness after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_metallic(self, 0.75) then\n"
    "        error('set_metallic failed')\n"
    "    end\n"
    "    if not near(engine.get_metallic(self), 0.75) then\n"
    "        error('get_metallic after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_albedo(self, 0.2, 0.4, 0.6) then\n"
    "        error('set_albedo failed')\n"
    "    end\n"
    "    local ax, ay, az = engine.get_albedo(self)\n"
    "    if not (near(ax, 0.2) and near(ay, 0.4) and near(az, 0.6)) then\n"
    "        error('get_albedo after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_opacity(self, 0.5) then\n"
    "        error('set_opacity failed')\n"
    "    end\n"
    "    if not near(engine.get_opacity(self), 0.5) then\n"
    "        error('get_opacity after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_mesh(self, 42) then\n"
    "        error('set_mesh failed')\n"
    "    end\n"
    "    if engine.get_mesh(self) ~= 42 then\n"
    "        error('get_mesh after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_light_color(self, 1.0, 0.5, 0.25) then\n"
    "        error('set_light_color failed')\n"
    "    end\n"
    "    local lr, lg, lb = engine.get_light_color(self)\n"
    "    if not (near(lr, 1.0) and near(lg, 0.5) and near(lb, 0.25)) then\n"
    "        error('get_light_color after set did not read the queued write')\n"
    "    end\n"
    "    if not engine.set_light_intensity(self, 3.0) then\n"
    "        error('set_light_intensity failed')\n"
    "    end\n"
    "    if not near(engine.get_light_intensity(self), 3.0) then\n"
    "        error('get_light_intensity after set did not read the queued '\n"
    "              .. 'write')\n"
    "    end\n"
    "    -- Queued removal/destruction must read as absent: later setters\n"
    "    -- report failure instead of resurrecting from a stale snapshot, and\n"
    "    -- getters must not resurrect the removed/destroyed state either.\n"
    "    if not engine.remove_light(self) then\n"
    "        error('remove_light failed')\n"
    "    end\n"
    "    if engine.get_light_intensity(self) ~= nil then\n"
    "        error('get_light_intensity after queued removal must read '\n"
    "              .. 'absent')\n"
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
    "    -- #125 boundary: a getter on an entity with a queued destroy reads\n"
    "    -- as absent rather than the still-live committed state.\n"
    "    if engine.get_restitution(doomed) ~= nil then\n"
    "        error('get_restitution on a queued-destroy entity must read '\n"
    "              .. 'absent')\n"
    "    end\n"
    "    if engine.get_position(doomed) ~= nil then\n"
    "        error('get_position on a queued-destroy entity must read absent')\n"
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
  static_cast<void>(engine::core::initialize_logging());
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

  // A plain (non-rigid-body) anchor and probe pair for the #125 get_parent
  // read-through boundary; reparenting the RigidBody-carrying `self` entity
  // would violate the hierarchy-root invariant, so this uses dedicated
  // entities instead.
  const rt::Entity anchor = world->create_scene_object();
  ctx.check(anchor != rt::kInvalidEntity, "create anchor entity");
  rt::NameComponent anchorName{};
  std::snprintf(anchorName.name, sizeof(anchorName.name), "Anchor");
  ctx.check(world->add_name_component(anchor, anchorName), "name anchor");

  const rt::Entity parentProbe = world->create_scene_object();
  ctx.check(parentProbe != rt::kInvalidEntity, "create parent probe entity");
  rt::NameComponent probeName{};
  std::snprintf(probeName.name, sizeof(probeName.name), "ParentProbe");
  ctx.check(world->add_name_component(parentProbe, probeName),
            "name parent probe");

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
  // set_mesh(self, 42) is one of the #125 get-after-set probes above, so the
  // asset id moves from its initial 7 to the queued 42, not "untouched".
  ctx.check(outMesh.meshAssetId == 42U, "mesh asset id updated by set_mesh");

  // The queued removal must win: the light is gone, not resurrected by the
  // rejected post-removal setter.
  ctx.check(!world->has_light_component(entity),
            "queued light removal is not resurrected");

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  static_cast<void>(std::remove(kModulePath));
  return ctx.finish("script_deferred_write");
}
