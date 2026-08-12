// Verifies engine.spawn_shape is transactional (issue #108): any failed
// component or payload insertion rolls the whole spawn back and returns
// nil to Lua instead of leaking a partial entity; the documented
// hull-exhaustion fallback still returns a fully valid box-collider
// entity; and a rolled-back spawn releases its convex-hull slot.

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

constexpr const char *kScriptPath = "spawn_shape_transaction_test.lua";

/// Writes contents to the temporary test script path.
bool write_script_file(const char *contents) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0 || file == nullptr) {
    return false;
  }
#else
  file = std::fopen(kScriptPath, "wb");
  if (file == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

// NaN color arguments make the mesh-component insertion fail its ingress
// validation, which is the reachable partial-failure injection for the
// production spawn path.
constexpr const char *kScript =
    "function spawn_nan_cube()\n"
    "    local e = engine.spawn_shape('cube', 0, 0, 0, 0/0, 1, 1)\n"
    "    if e ~= nil then error('NaN-albedo cube spawn must fail') end\n"
    "end\n"
    "function spawn_nan_cylinder()\n"
    "    local e = engine.spawn_shape('cylinder', 1, 0, 0, 0/0, 1, 1)\n"
    "    if e ~= nil then error('NaN-albedo cylinder spawn must fail') end\n"
    "end\n"
    "function fill_hull_slots()\n"
    "    for i = 1, 256 do\n"
    "        local e = engine.spawn_shape('cylinder', 0, 0, 0)\n"
    "        if e == nil then error('cylinder ' .. i .. ' failed') end\n"
    "        if i == 256 then engine.set_name(e, 'cyl_last') end\n"
    "    end\n"
    "end\n"
    "function spawn_fallback_cylinder()\n"
    "    local e = engine.spawn_shape('cylinder', 0, 0, 0)\n"
    "    if e == nil then error('hull-exhausted spawn must fall back') end\n"
    "    engine.set_name(e, 'fallback_cylinder')\n"
    "end\n"
    "function spawn_at_capacity()\n"
    "    local e = engine.spawn_shape('cube', 0, 0, 0)\n"
    "    if e ~= nil then error('capacity-exhausted spawn must fail') end\n"
    "end\n";

/// Finds the alive entity carrying the given name (kInvalidEntity when
/// absent).
rt::Entity find_named(rt::World &world, const char *name) noexcept {
  rt::Entity found = rt::kInvalidEntity;
  world.for_each_alive([&](rt::Entity entity) noexcept {
    rt::NameComponent nameComponent{};
    if (world.get_name_component(entity, &nameComponent) &&
        (std::strcmp(nameComponent.name, name) == 0)) {
      found = entity;
    }
  });
  return found;
}

/// True when the named spawn owns the full shape kit: rigid body, a
/// collider of the expected shape/provenance, and a mesh component.
bool spawn_is_complete(rt::World &world, const char *name,
                       rt::ColliderShape shape,
                       rt::HullSource hullSource) noexcept {
  const rt::Entity entity = find_named(world, name);
  if (entity == rt::kInvalidEntity) {
    return false;
  }
  rt::RigidBody body{};
  rt::Collider collider{};
  rt::MeshComponent mesh{};
  return world.get_rigid_body(entity, &body) &&
         world.get_collider(entity, &collider) && (collider.shape == shape) &&
         (collider.hullSource == hullSource) &&
         world.get_mesh_component(entity, &mesh);
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
  sc::set_default_mesh_asset_id(777ULL);

  engine::tests::TestContext ctx;
  ctx.check(write_script_file(kScript), "write test script");
  ctx.check(sc::load_script(kScriptPath), "load test script");

  // A failed mesh insertion returns nil and leaves no partial entity.
  const std::size_t aliveBefore = world->alive_entity_count();
  ctx.check(sc::call_script_function("spawn_nan_cube"),
            "NaN-albedo cube spawn reports failure to Lua");
  ctx.check(world->alive_entity_count() == aliveBefore,
            "failed cube spawn leaves no partial entity");

  // A rolled-back cylinder spawn must release its hull slot: afterwards
  // all 256 fixed hull slots are still mintable as real convex hulls.
  ctx.check(sc::call_script_function("spawn_nan_cylinder"),
            "NaN-albedo cylinder spawn reports failure to Lua");
  ctx.check(world->alive_entity_count() == aliveBefore,
            "failed cylinder spawn leaves no partial entity");
  ctx.check(sc::call_script_function("fill_hull_slots"),
            "all fixed hull slots remain available after the rollback");
  ctx.check(spawn_is_complete(*world, "cyl_last",
                              rt::ColliderShape::ConvexHull,
                              rt::HullSource::Cylinder),
            "last hull-slot cylinder is a complete convex-hull spawn");

  // Hull exhaustion takes the documented fallback: a fully valid
  // box-collider entity, never a partial or hull-inconsistent one.
  ctx.check(sc::call_script_function("spawn_fallback_cylinder"),
            "hull-exhausted cylinder spawn returns a valid entity");
  ctx.check(spawn_is_complete(*world, "fallback_cylinder",
                              rt::ColliderShape::AABB, rt::HullSource::None),
            "hull-exhausted spawn is a complete box-collider entity");

  // Entity-capacity exhaustion also reports failure without leaks.
  while (world->create_scene_object(rt::Transform{}) != rt::kInvalidEntity) {
  }
  const std::size_t aliveAtCapacity = world->alive_entity_count();
  ctx.check(sc::call_script_function("spawn_at_capacity"),
            "capacity-exhausted spawn reports failure to Lua");
  ctx.check(world->alive_entity_count() == aliveAtCapacity,
            "capacity-exhausted spawn leaves the entity count unchanged");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("spawn_shape_transaction");
}
