// Verifies a built-in primitive's collider is described once
// (runtime::primitive_collider) and that engine.spawn_shape installs that
// description: every shape spawned through the Lua binding carries exactly
// the owner's shape, hull provenance, extents and offset. A plane's box
// sits below its surface, so its top is flush with the ground that is
// drawn (issue #729: the Lua path centred it on the surface instead, and
// whatever stood on a spawned plane hovered 0.1 m above it).

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/renderer/mesh_primitives.h"
#include "engine/runtime/primitive_collider.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kScriptPath = "primitive_collider_test.lua";

constexpr const char *kScript =
    "function spawn_all()\n"
    "    for _, shape in ipairs({'cube', 'sphere', 'cylinder', 'capsule',\n"
    "                            'pyramid', 'plane'}) do\n"
    "        local e = engine.spawn_shape(shape, 0, 0, 0)\n"
    "        if e == nil then error(shape .. ' did not spawn') end\n"
    "        engine.set_name(e, shape)\n"
    "    end\n"
    "end\n";

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

/// The collider of the alive entity named `name`; false when there is none.
bool named_collider(rt::World &world, const char *name,
                    rt::Collider *out) noexcept {
  bool found = false;
  world.for_each_alive([&](rt::Entity entity) noexcept {
    rt::NameComponent nameComponent{};
    if (!found && world.get_name_component(entity, &nameComponent) &&
        (std::strcmp(nameComponent.name, name) == 0)) {
      found = world.get_collider(entity, out);
    }
  });
  return found;
}

bool same_vec3(const engine::math::Vec3 &a,
               const engine::math::Vec3 &b) noexcept {
  return (a.x == b.x) && (a.y == b.y) && (a.z == b.z);
}

/// True when `installed` is exactly the owner's collider for `shape`.
bool matches_owner(const rt::Collider &installed,
                   rt::PrimitiveShape shape) noexcept {
  const rt::Collider owner = rt::primitive_collider(shape);
  return (installed.shape == owner.shape) &&
         (installed.hullSource == owner.hullSource) &&
         same_vec3(installed.halfExtents, owner.halfExtents) &&
         same_vec3(installed.localPosition, owner.localPosition);
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
  ctx.check(sc::call_script_function("spawn_all"),
            "every primitive spawns through engine.spawn_shape");

  rt::Collider plane{};
  ctx.check(named_collider(*world, "plane", &plane) &&
                ((plane.localPosition.y + plane.halfExtents.y) ==
                 engine::renderer::kBuiltinPlaneSurfaceY),
            "a spawned plane's collider top is its drawn surface");

  // Every shape the binding spawns carries the owner's collider exactly.
  const struct {
    const char *name;
    rt::PrimitiveShape shape;
  } shapes[] = {{"cube", rt::PrimitiveShape::Cube},
                {"sphere", rt::PrimitiveShape::Sphere},
                {"cylinder", rt::PrimitiveShape::Cylinder},
                {"capsule", rt::PrimitiveShape::Capsule},
                {"pyramid", rt::PrimitiveShape::Pyramid},
                {"plane", rt::PrimitiveShape::Plane}};
  bool allMatch = true;
  for (const auto &entry : shapes) {
    rt::Collider installed{};
    const bool match = named_collider(*world, entry.name, &installed) &&
                       matches_owner(installed, entry.shape);
    if (!match) {
      std::fprintf(stderr, "FAIL: spawned %s differs from its owner\n",
                   entry.name);
    }
    allMatch = allMatch && match;
  }
  ctx.check(allMatch, "spawn_shape installs runtime::primitive_collider");

  // The owner's description: the hull primitives carry their provenance,
  // the others their box, sphere or capsule, and only the plane an offset.
  ctx.check(
      (rt::primitive_collider(rt::PrimitiveShape::Cylinder).hullSource ==
       rt::HullSource::Cylinder) &&
          (rt::primitive_collider(rt::PrimitiveShape::Pyramid).hullSource ==
           rt::HullSource::Pyramid) &&
          (rt::primitive_collider(rt::PrimitiveShape::Sphere).shape ==
           rt::ColliderShape::Sphere) &&
          (rt::primitive_collider(rt::PrimitiveShape::Capsule).shape ==
           rt::ColliderShape::Capsule) &&
          (rt::primitive_collider(rt::PrimitiveShape::Cube).shape ==
           rt::ColliderShape::AABB),
      "each primitive has its own collider shape");
  const rt::Collider ownerPlane =
      rt::primitive_collider(rt::PrimitiveShape::Plane);
  ctx.check((ownerPlane.shape == rt::ColliderShape::AABB) &&
                (ownerPlane.halfExtents.x == 5.0F) &&
                (ownerPlane.halfExtents.z == 5.0F) &&
                (ownerPlane.localPosition.y + ownerPlane.halfExtents.y ==
                 engine::renderer::kBuiltinPlaneSurfaceY) &&
                (ownerPlane.localPosition.y < 0.0F),
            "the plane is a 10 m box wholly below its surface");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("primitive_collider");
}
