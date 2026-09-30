// Pins render-prep culling to what is drawn: a mesh is culled by its own
// object-space bounds under its entity's transform, never by a unit cube
// at the entity's origin or by its collider. Through the production
// pipeline, against a camera whose frustum is 5.77 wide either side of
// the origin at the subjects' depth: a mesh whose geometry sits offset
// from its origin, a mesh larger than the unit cube, a big mesh with a
// small collider, a foliage patch scaled by its transform, and a foliage
// instance the wind sways into view are each drawn, and a mesh wholly
// outside the frustum is still culled. The editor's Frame Selected reads
// the same bounds through its bridge.

#include "../render_prep_harness.h"
#include "engine/core/job_system.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using engine::math::Vec3;
using engine::runtime::Entity;

/// Registers a mesh with the given object-space bounds under `path`.
engine::content::AssetId
register_bounded_mesh(engine::renderer::AssetDatabase *database,
                      engine::renderer::GpuMeshRegistry *registry,
                      const char *path, const Vec3 &center,
                      const Vec3 &half) noexcept {
  engine::renderer::GpuMesh mesh{};
  mesh.vertexCount = 3U;
  mesh.boundsCenter = center;
  mesh.boundsHalfExtents = half;
  const engine::renderer::MeshHandle handle =
      engine::renderer::register_gpu_mesh(registry, mesh);
  const engine::content::AssetId id =
      engine::content::make_asset_id_from_path(path);
  if ((handle == engine::renderer::kInvalidMeshHandle) ||
      (id == engine::content::kInvalidAssetId) ||
      !engine::renderer::register_mesh_asset(database, id, path, handle)) {
    return engine::content::kInvalidAssetId;
  }
  return id;
}

/// A mesh entity at `position`, with a box collider of `colliderHalf`
/// unless that is zero.
Entity add_mesh_entity(engine::runtime::World &world,
                       engine::content::AssetId mesh, const Vec3 &position,
                       const Vec3 &colliderHalf) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const Entity entity = world.create_scene_object(transform);
  engine::runtime::MeshComponent component{};
  component.meshAssetId = mesh;
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world.add_mesh_component(entity, component)) {
    return engine::runtime::kInvalidEntity;
  }
  if (colliderHalf.x > 0.0F) {
    engine::runtime::Collider collider{};
    collider.halfExtents = colliderHalf;
    if (!world.add_collider(entity, collider)) {
      return engine::runtime::kInvalidEntity;
    }
  }
  return entity;
}

/// A foliage patch of one instance at `offset` under a transform at
/// `position` with uniform `scale`.
Entity add_foliage_entity(engine::runtime::World &world,
                          engine::content::AssetId mesh, const Vec3 &position,
                          float scale, const Vec3 &offset,
                          float windStrength) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  transform.scale = Vec3(scale, scale, scale);
  const Entity entity = world.create_scene_object(transform);
  engine::runtime::FoliagePatchComponent foliage{};
  foliage.meshAssetIds[0] = mesh;
  foliage.instanceCount = 1U;
  foliage.instances[0].offset = offset;
  foliage.windStrength = windStrength;
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world.add_foliage_patch_component(entity, foliage)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

bool drawn(const engine::renderer::CommandBufferView &view,
           Entity entity) noexcept {
  for (std::uint32_t i = 0U; i < view.count; ++i) {
    if (view.data[i].entity == entity.index) {
      return true;
    }
  }
  return false;
}

} // namespace

/// Runs this executable or test program.
/// The editor reads the bounds render prep culls by, through the same
/// peek: the offset mesh reports its centre (20, 0, 0) and unit half
/// extents exactly. An unknown mesh, and any mesh while the registry is
/// unpublished, report nothing and leave the outputs alone.
int check_editor_mesh_bounds(engine::renderer::AssetDatabase *database,
                             const engine::renderer::GpuMeshRegistry *registry,
                             engine::content::AssetId offsetMesh) noexcept {
  engine::runtime::EngineAssetDatabaseService service{};
  service.database = database;
  engine::runtime::set_editor_asset_service(&service);
  engine::runtime::set_editor_mesh_registry(registry);
  Vec3 center(-1.0F, -1.0F, -1.0F);
  Vec3 half(-1.0F, -1.0F, -1.0F);
  int result = 0;
  if (!engine::runtime::editor_mesh_local_bounds(offsetMesh, &center, &half) ||
      (center.x != 20.0F) || (center.y != 0.0F) || (center.z != 0.0F) ||
      (half.x != 1.0F) || (half.y != 1.0F) || (half.z != 1.0F)) {
    std::fprintf(stderr, "FAIL: the editor reads the mesh's bounds\n");
    result = 20;
  }
  Vec3 untouched(-1.0F, -1.0F, -1.0F);
  if (engine::runtime::editor_mesh_local_bounds(12345U, &untouched, &half) ||
      (untouched.x != -1.0F)) {
    std::fprintf(stderr, "FAIL: an unknown mesh reports no bounds\n");
    result = 21;
  }
  engine::runtime::set_editor_mesh_registry(nullptr);
  if (engine::runtime::editor_mesh_local_bounds(offsetMesh, &untouched,
                                                &half) ||
      (untouched.x != -1.0F)) {
    std::fprintf(stderr, "FAIL: no registry, no bounds\n");
    result = 22;
  }
  engine::runtime::set_editor_asset_service(nullptr);
  return result;
}

int main() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  std::unique_ptr<engine::renderer::AssetDatabase> assetDatabase(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> meshRegistry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  std::unique_ptr<engine::renderer::CommandBufferBuilder> commandBuffer(
      new (std::nothrow) engine::renderer::CommandBufferBuilder());
  std::unique_ptr<engine::runtime::RenderPrepPipelineContext> prepContext(
      new (std::nothrow) engine::runtime::RenderPrepPipelineContext());
  if ((world == nullptr) || (assetDatabase == nullptr) ||
      (meshRegistry == nullptr) || (commandBuffer == nullptr) ||
      (prepContext == nullptr)) {
    return 1;
  }
  engine::renderer::clear_asset_database(assetDatabase.get());
  engine::renderer::AssetDatabase *db = assetDatabase.get();
  engine::renderer::GpuMeshRegistry *registry = meshRegistry.get();

  // Geometry 20 units along +x from the mesh origin, a 100-unit-wide
  // mesh, a 20-unit-wide mesh, and a unit mesh for foliage.
  const engine::content::AssetId offsetMesh =
      register_bounded_mesh(db, registry, "integration://bounds-offset.mesh",
                            Vec3(20.0F, 0.0F, 0.0F), Vec3(1.0F, 1.0F, 1.0F));
  const engine::content::AssetId wideMesh =
      register_bounded_mesh(db, registry, "integration://bounds-wide.mesh",
                            Vec3(0.0F, 0.0F, 0.0F), Vec3(50.0F, 1.0F, 1.0F));
  const engine::content::AssetId bigMesh =
      register_bounded_mesh(db, registry, "integration://bounds-big.mesh",
                            Vec3(0.0F, 0.0F, 0.0F), Vec3(10.0F, 1.0F, 1.0F));
  const engine::content::AssetId unitMesh =
      register_bounded_mesh(db, registry, "integration://bounds-unit.mesh",
                            Vec3(0.0F, 0.0F, 0.0F), Vec3(0.5F, 0.5F, 0.5F));
  if ((offsetMesh == engine::content::kInvalidAssetId) ||
      (wideMesh == engine::content::kInvalidAssetId) ||
      (bigMesh == engine::content::kInvalidAssetId) ||
      (unitMesh == engine::content::kInvalidAssetId)) {
    return 2;
  }

  const Vec3 noCollider(0.0F, 0.0F, 0.0F);
  struct Case final {
    const char *what;
    Entity entity;
    bool visible;
  };
  // Each subject's origin, or its unit cube, or its collider, lies wholly
  // outside the frustum (|x| > 5.77 + the half extent); only its drawn
  // geometry reaches in.
  const Case cases[] = {
      {"geometry offset from its origin into view",
       add_mesh_entity(*world, offsetMesh, Vec3(-20.0F, 0.0F, 0.0F),
                       noCollider),
       true},
      {"a mesh wider than the unit cube",
       add_mesh_entity(*world, wideMesh, Vec3(30.0F, 0.0F, 0.0F), noCollider),
       true},
      {"a big mesh with a small collider",
       add_mesh_entity(*world, bigMesh, Vec3(12.0F, 0.0F, 0.0F),
                       Vec3(0.5F, 0.5F, 0.5F)),
       true},
      {"a mesh wholly outside the frustum",
       add_mesh_entity(*world, unitMesh, Vec3(40.0F, 0.0F, 0.0F), noCollider),
       false},
      // Scale 20: the instance centre lands at x = -12 + 20 * 0.25 = -7,
      // and its unit mesh spans 10 units either side of it.
      {"a foliage instance scaled by its patch",
       add_foliage_entity(*world, unitMesh, Vec3(-12.0F, 0.0F, 0.0F), 20.0F,
                          Vec3(0.25F, 0.0F, 0.0F), 0.0F),
       true},
      // Half a unit wide at x = 7.3, 1.03 outside; the wind sways it up to
      // 3 units along x.
      {"a foliage instance the wind sways into view",
       add_foliage_entity(*world, unitMesh, Vec3(7.3F, 0.0F, 0.0F), 1.0F,
                          Vec3(0.0F, 0.0F, 0.0F), 3.0F),
       true},
  };
  for (const Case &row : cases) {
    if (row.entity == engine::runtime::kInvalidEntity) {
      std::fprintf(stderr, "could not author: %s\n", row.what);
      return 3;
    }
  }

  if (!engine::core::initialize_job_system(0U)) {
    return 4;
  }
  engine::renderer::CameraState camera{};
  camera.position = Vec3(0.0F, 0.0F, 10.0F);
  camera.target = Vec3(0.0F, 0.0F, 0.0F);
  const engine::runtime::RenderPrepView prepView =
      engine::runtime::make_render_prep_view(camera, 1.0F);
  const bool prepared = engine::tests::run_render_prep(
      world.get(), prepContext.get(), commandBuffer.get(), assetDatabase.get(),
      meshRegistry.get(), prepView);
  engine::core::shutdown_job_system();
  if (!prepared) {
    return 5;
  }

  const engine::renderer::CommandBufferView view = commandBuffer->view();
  int failures = 0;
  for (const Case &row : cases) {
    if (drawn(view, row.entity) != row.visible) {
      std::fprintf(stderr, "FAIL: %s was %s\n", row.what,
                   row.visible ? "culled" : "drawn");
      ++failures;
    }
  }
  if (failures != 0) {
    return 10;
  }
  return check_editor_mesh_bounds(db, registry, offsetMesh);
}
