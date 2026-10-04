// Verifies a TriMesh collider end to end through the production pipeline:
// on the sample project, a collider naming the cooked platform_square mesh
// (scaled five times, so its top is at y = 1) gets that mesh from the
// collider mesh pass. A sphere and a capsule dropped onto it come to rest
// on its top, a ray and a sphere sweep meet its top, and the World's state
// hash is the same at 1, 2 and 8 worker threads. A collider naming a mesh
// no asset has stays empty, and a body over it falls.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "engine/content/asset_identity.h"
#include "engine/engine.h"
#include "engine/physics/physics_query.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "../asset_root.h"

namespace {

namespace math = engine::math;
namespace runtime = engine::runtime;

constexpr float kFrameSeconds = 1.0F / 60.0F;
constexpr std::uint32_t kFrameCount = 300U;
/// platform_square.mesh: its source GUID and the local id its cook stamp
/// gives the mesh.
constexpr const char *kPlatformRef =
    "cd16eb58-97de-4b55-bf55-aa193a64392c#432408a2e33116bc";
/// A GUID no sample asset has.
constexpr const char *kMissingRef = "10000000-0000-4000-8000-0000000000ff";
/// The platform's top: its mesh's top (0.2) at a scale of 5.
constexpr float kPlatformTop = 1.0F;

struct Fixture final {
  runtime::Entity platform = runtime::kInvalidEntity;
  runtime::Entity missing = runtime::kInvalidEntity;
  runtime::Entity sphere = runtime::kInvalidEntity;
  runtime::Entity capsule = runtime::kInvalidEntity;
  runtime::Entity faller = runtime::kInvalidEntity;
};

struct Outcome final {
  std::uint64_t hash = 0U;
  math::Vec3 sphere{};
  math::Vec3 capsule{};
  math::Vec3 faller{};
  float rayDistance = -1.0F;
  float sweepDistance = -1.0F;
  bool platformMesh = false;
  bool missingMesh = true;
};

engine::core::AssetRef ref_of(const char *text) {
  engine::core::AssetRef ref{};
  static_cast<void>(engine::content::parse_asset_ref(text, &ref));
  return ref;
}

runtime::Entity add_mesh(runtime::World &world, const math::Vec3 &position,
                         const char *ref) {
  runtime::Transform transform{};
  transform.position = position;
  transform.scale = math::Vec3(5.0F, 5.0F, 5.0F);
  const runtime::Entity entity = world.create_scene_object(transform);
  runtime::Collider collider{};
  collider.shape = runtime::ColliderShape::TriMesh;
  collider.meshRef = ref_of(ref);
  return ((entity != runtime::kInvalidEntity) &&
          world.add_collider(entity, collider))
             ? entity
             : runtime::kInvalidEntity;
}

runtime::Entity add_body(runtime::World &world, runtime::ColliderShape shape,
                         const math::Vec3 &halfExtents,
                         const math::Vec3 &position) {
  runtime::Transform transform{};
  transform.position = position;
  const runtime::Entity entity = world.create_scene_object(transform);
  runtime::Collider collider{};
  collider.shape = shape;
  collider.halfExtents = halfExtents;
  collider.restitution = 0.0F;
  collider.staticFriction = 0.9F;
  collider.dynamicFriction = 0.7F;
  runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  body.acceleration = math::Vec3(0.0F, -9.8F, 0.0F);
  return ((entity != runtime::kInvalidEntity) &&
          world.add_collider(entity, collider) &&
          world.add_rigid_body(entity, body))
             ? entity
             : runtime::kInvalidEntity;
}

bool populate(runtime::World &world, Fixture *out) {
  out->platform = add_mesh(world, math::Vec3(0.0F, 0.0F, 0.0F), kPlatformRef);
  out->missing = add_mesh(world, math::Vec3(20.0F, 0.0F, 0.0F), kMissingRef);
  out->sphere =
      add_body(world, runtime::ColliderShape::Sphere,
               math::Vec3(0.5F, 0.5F, 0.5F), math::Vec3(-1.0F, 4.0F, 0.5F));
  out->capsule =
      add_body(world, runtime::ColliderShape::Capsule,
               math::Vec3(0.3F, 0.5F, 0.3F), math::Vec3(1.5F, 4.0F, -1.0F));
  out->faller =
      add_body(world, runtime::ColliderShape::Sphere,
               math::Vec3(0.5F, 0.5F, 0.5F), math::Vec3(20.0F, 4.0F, 0.0F));
  return (out->platform != runtime::kInvalidEntity) &&
         (out->missing != runtime::kInvalidEntity) &&
         (out->sphere != runtime::kInvalidEntity) &&
         (out->capsule != runtime::kInvalidEntity) &&
         (out->faller != runtime::kInvalidEntity);
}

bool position_of(runtime::World &world, runtime::Entity entity,
                 math::Vec3 *out) {
  runtime::Transform transform{};
  if (!world.get_transform(entity, &transform)) {
    return false;
  }
  *out = transform.position;
  return true;
}

bool run(std::uint32_t workers, Outcome *out) {
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = workers;
  if (!engine::bootstrap(config)) {
    std::printf("FAIL: bootstrap with %u workers\n", workers);
    return false;
  }
  bool ok = false;
  {
    engine::EnginePipeline pipeline;
    runtime::World *world = nullptr;
    Fixture fixture{};
    if (pipeline.initialize(0U) && ((world = pipeline.world()) != nullptr)) {
      runtime::reset_world(*world);
      ok = populate(*world, &fixture) &&
           pipeline.set_frame_delta_override(kFrameSeconds);
    }
    for (std::uint32_t frame = 0U; ok && (frame < kFrameCount); ++frame) {
      ok = pipeline.execute_frame();
    }
    ok = ok && position_of(*world, fixture.sphere, &out->sphere) &&
         position_of(*world, fixture.capsule, &out->capsule) &&
         position_of(*world, fixture.faller, &out->faller);
    if (ok) {
      out->hash = world->state_hash(nullptr);
      out->platformMesh =
          runtime::get_tri_mesh_data(*world, fixture.platform) != nullptr;
      out->missingMesh =
          runtime::get_tri_mesh_data(*world, fixture.missing) != nullptr;
      engine::physics::PhysicsRaycastHit ray{};
      if (runtime::raycast(*world, math::Vec3(0.0F, 5.0F, 3.0F),
                           math::Vec3(0.0F, -1.0F, 0.0F), 10.0F, &ray) &&
          (ray.entity == fixture.platform)) {
        out->rayDistance = ray.distance;
      }
      engine::physics::SweepHit sweep{};
      if (runtime::sweep_sphere(*world, math::Vec3(-3.0F, 5.0F, -3.0F), 0.5F,
                                math::Vec3(0.0F, -1.0F, 0.0F), 10.0F, &sweep) &&
          (sweep.entityIndex == fixture.platform.index)) {
        out->sweepDistance = sweep.distance;
      }
    }
    pipeline.teardown();
  }
  engine::shutdown();
  return ok;
}

int g_failures = 0;

void check(bool condition, const char *name) {
  if (!condition) {
    std::printf("FAIL: %s\n", name);
    ++g_failures;
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::printf("FAIL: test setup\n");
    return 1;
  }
  Outcome outcomes[3] = {};
  const std::uint32_t workerCounts[3] = {1U, 2U, 8U};
  for (std::size_t i = 0U; i < 3U; ++i) {
    if (!run(workerCounts[i], &outcomes[i])) {
      std::printf("FAIL: pipeline run with %u workers\n", workerCounts[i]);
      return 1;
    }
  }
  const Outcome &o = outcomes[0];
  std::printf("[tri-mesh] sphere y=%g capsule y=%g faller y=%g ray=%g "
              "sweep=%g hash=%016llx\n",
              static_cast<double>(o.sphere.y), static_cast<double>(o.capsule.y),
              static_cast<double>(o.faller.y),
              static_cast<double>(o.rayDistance),
              static_cast<double>(o.sweepDistance),
              static_cast<unsigned long long>(o.hash));
  check(o.platformMesh, "the platform collider gets its mesh");
  check(!o.missingMesh, "a collider naming no asset gets no mesh");
  // The rest suite's tolerances: under 1 cm off a body's resting height.
  check(std::fabs(o.sphere.y - (kPlatformTop + 0.5F)) < 0.01F,
        "a sphere rests on the mesh platform's top");
  check(std::fabs(o.capsule.y - (kPlatformTop + 0.8F)) < 0.01F,
        "an upright capsule rests on the mesh platform's top");
  check(o.faller.y < -1.0F, "a body over the empty collider falls");
  check(std::fabs(o.rayDistance - (5.0F - kPlatformTop)) < 1.0e-4F,
        "a ray down meets the platform's top");
  // Conservative advancement stops within a few 1e-5 steps of the surface.
  check(std::fabs(o.sweepDistance - (5.0F - kPlatformTop - 0.5F)) < 1.0e-4F,
        "a sphere swept down stops on the platform's top");
  for (std::size_t i = 1U; i < 3U; ++i) {
    check(outcomes[i].hash == o.hash,
          "the state hash is the same at 1, 2 and 8 workers");
  }
  if (g_failures == 0) {
    std::printf("PASS: tri mesh pipeline\n");
  }
  return (g_failures == 0) ? 0 : 1;
}
