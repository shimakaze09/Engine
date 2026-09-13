// Verifies that script-driven point and spot light mutations reach the
// World only through the runtime bridge's RuntimeServices ops (#309): the
// ops carry the full entity handle, so a handle whose generation the
// World has recycled is refused rather than re-targeted to the index's
// new occupant; the immediate (Input-phase) Lua path and the deferred
// (BeginPlay-queued, flushed) Lua path both apply through the same ops.

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

constexpr const char *kScriptPath = "script_light_bridge_test.lua";
constexpr const char *kModulePath = "script_light_bridge_module.lua";

/// Writes contents to a relative path (fixture side only).
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

// Main script: the immediate path adds and removes lights while the World
// sits in the Input phase, so every call applies through the bridge op at
// once and the getter reads it back in the same call.
constexpr const char *kScript =
    "g_lit = nil\n"
    "function setup_immediate()\n"
    "    g_lit = engine.spawn_entity()\n"
    "    if g_lit == nil then error('spawn failed') end\n"
    "    if not engine.add_point_light(g_lit, 0.25, 0.5, 0.75, 2.0, 7.0) then\n"
    "        error('add_point_light failed')\n"
    "    end\n"
    "    local r, g, b, i, rad = engine.get_point_light(g_lit)\n"
    "    if r ~= 0.25 or g ~= 0.5 or b ~= 0.75 or i ~= 2.0 or rad ~= 7.0 then\n"
    "        error('point light did not read back')\n"
    "    end\n"
    "    if not engine.add_spot_light(g_lit, 1.0, 0.5, 0.25, 0.0, -1.0, 0.0,\n"
    "                                 3.0, 9.0, 0.25, 0.5) then\n"
    "        error('add_spot_light failed')\n"
    "    end\n"
    "    local sr, sg, sb, dx, dy, dz, si, srad, inner, outer =\n"
    "        engine.get_spot_light(g_lit)\n"
    "    if sr ~= 1.0 or sg ~= 0.5 or sb ~= 0.25 or dx ~= 0.0 or dy ~= -1.0\n"
    "       or dz ~= 0.0 or si ~= 3.0 or srad ~= 9.0 or inner ~= 0.25\n"
    "       or outer ~= 0.5 then\n"
    "        error('spot light did not read back')\n"
    "    end\n"
    "    if not engine.remove_spot_light(g_lit) then\n"
    "        error('remove_spot_light failed')\n"
    "    end\n"
    "    if engine.get_spot_light(g_lit) ~= nil then\n"
    "        error('spot light survived removal')\n"
    "    end\n"
    "    if engine.remove_spot_light(g_lit) then\n"
    "        error('second removal reported success')\n"
    "    end\n"
    "end\n"
    "function verify_begin_play_completed()\n"
    "    if bp_completed ~= true then\n"
    "        error('on_begin_play did not run to completion')\n"
    "    end\n"
    "end\n";

// Entity module: on_begin_play runs outside the Input phase, so both calls
// queue and apply only at the flush that follows the phase.
constexpr const char *kModule =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    if not engine.add_point_light(self, 0.1, 0.2, 0.3, 4.0, 11.0) then\n"
    "        error('queued add_point_light failed')\n"
    "    end\n"
    "    if not engine.remove_spot_light(self) then\n"
    "        error('queued remove_spot_light failed')\n"
    "    end\n"
    "    bp_completed = true\n"
    "end\n"
    "return M\n";

/// Creates a scene object carrying the module as its entity script.
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

/// The bridge ops refuse a handle whose generation the World has recycled:
/// after destroying an entity and creating another on the same index, the
/// stale handle neither adds to nor removes from the new occupant.
void check_stale_handle_refused(engine::tests::TestContext &ctx,
                                rt::World *world,
                                const sc::RuntimeServices *services) noexcept {
  const rt::Entity first = world->create_scene_object();
  ctx.check(first != rt::kInvalidEntity, "create first entity");

  rt::PointLightComponent point{};
  point.intensity = 5.0F;
  ctx.check(services->add_point_light_component_op(world, first, point),
            "bridge adds a point light to a live entity");
  rt::SpotLightComponent spot{};
  spot.intensity = 6.0F;
  ctx.check(services->add_spot_light_component_op(world, first, spot),
            "bridge adds a spot light to a live entity");

  ctx.check(world->destroy_entity(first), "destroy first entity");
  const rt::Entity second = world->create_scene_object();
  ctx.check(second != rt::kInvalidEntity, "create second entity");
  ctx.check(second.index == first.index,
            "second entity recycles the first entity's index");
  ctx.check(second.generation != first.generation,
            "second entity carries a new generation");

  ctx.check(!services->add_point_light_component_op(world, first, point),
            "stale handle cannot add a point light");
  ctx.check(!services->add_spot_light_component_op(world, first, spot),
            "stale handle cannot add a spot light");
  ctx.check(!world->has_point_light_component(second),
            "index occupant gained no point light from the stale handle");
  ctx.check(!world->has_spot_light_component(second),
            "index occupant gained no spot light from the stale handle");

  ctx.check(services->add_point_light_component_op(world, second, point),
            "live handle adds a point light to the occupant");
  ctx.check(services->add_spot_light_component_op(world, second, spot),
            "live handle adds a spot light to the occupant");
  ctx.check(!services->remove_point_light_component_op(world, first),
            "stale handle cannot remove the occupant's point light");
  ctx.check(!services->remove_spot_light_component_op(world, first),
            "stale handle cannot remove the occupant's spot light");
  ctx.check(world->has_point_light_component(second),
            "occupant keeps its point light");
  ctx.check(world->has_spot_light_component(second),
            "occupant keeps its spot light");
  ctx.check(services->remove_point_light_component_op(world, second),
            "live handle removes the point light");
  ctx.check(services->remove_spot_light_component_op(world, second),
            "live handle removes the spot light");
  ctx.check(!services->add_point_light_component_op(nullptr, second, point),
            "null world is refused");
  ctx.check(!services->remove_spot_light_component_op(nullptr, second),
            "null world is refused for removal");
}

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
  const auto *services = serviceLocator.get_service<sc::RuntimeServices>();
  ctx.check(services != nullptr, "runtime services published");
  if (services != nullptr) {
    ctx.check((services->add_point_light_component_op != nullptr) &&
                  (services->remove_point_light_component_op != nullptr) &&
                  (services->add_spot_light_component_op != nullptr) &&
                  (services->remove_spot_light_component_op != nullptr),
              "runtime publishes all four light ops");
    check_stale_handle_refused(ctx, world.get(), services);
  }

  ctx.check(write_file_at(kScriptPath, kScript), "write main script");
  ctx.check(sc::load_script(kScriptPath), "load main script");
  ctx.check(sc::call_script_function("setup_immediate"),
            "immediate path adds, reads back, and removes through the bridge");

  ctx.check(write_file_at(kModulePath, kModule), "write script module");
  const rt::Entity scripted = make_scripted_entity(world.get(), kModulePath);
  ctx.check(scripted != rt::kInvalidEntity, "create scripted entity");
  rt::SpotLightComponent preSpot{};
  ctx.check(world->add_spot_light_component(scripted, preSpot),
            "scripted entity starts with a spot light to remove");

  run_begin_play_phase(world.get());

  ctx.check(sc::call_script_function("verify_begin_play_completed"),
            "on_begin_play ran to completion");
  rt::PointLightComponent queuedPoint{};
  ctx.check(world->get_point_light_component(scripted, &queuedPoint),
            "queued add_point_light applied at flush");
  ctx.check((queuedPoint.color.x == 0.1F) && (queuedPoint.color.y == 0.2F) &&
                (queuedPoint.color.z == 0.3F) &&
                (queuedPoint.intensity == 4.0F) &&
                (queuedPoint.radius == 11.0F),
            "queued point light carries the scripted values");
  ctx.check(!world->has_spot_light_component(scripted),
            "queued remove_spot_light applied at flush");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  static_cast<void>(std::remove(kModulePath));
  return ctx.finish("script_light_bridge");
}
