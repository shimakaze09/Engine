// Pins the depth order render prep gives draws, through the production
// pipeline, for every camera a scene can author. Transparent draws must
// reach the flush back to front and opaque ones front to back, so the key's
// depth field has to measure distance along the view under an orthographic
// projection as well as a perspective one, and over the camera's own depth
// range. It used to read the clip-space w, which an orthographic projection
// sets to 1 everywhere, and normalise it over a fixed 200 units, so an
// orthographic view and anything past 200 units got one depth for every
// draw and fell back to entity order.
//
// Each case authors the draws so that entity order is the wrong answer:
// with no depth information the tiebreak alone would put them the wrong
// way round.

#include "../render_prep_harness.h"
#include "engine/core/job_system.h"
#include "engine/math/vec3.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/world.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using engine::math::Vec3;
using engine::renderer::CameraState;

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

constexpr std::uint64_t kDepthMask = 0xFFFFULL;

/// The fixture every case prepares a fresh world in.
struct Fixture final {
  std::unique_ptr<engine::runtime::World> world;
  std::unique_ptr<engine::renderer::AssetDatabase> assetDatabase;
  std::unique_ptr<engine::renderer::GpuMeshRegistry> meshRegistry;
  std::unique_ptr<engine::renderer::CommandBufferBuilder> commandBuffer;
  std::unique_ptr<engine::runtime::RenderPrepPipelineContext> prepContext;
  engine::content::AssetId meshAssetId = engine::content::kInvalidAssetId;
  engine::content::AssetId opaqueMaterial = engine::content::kInvalidAssetId;
  engine::content::AssetId blendMaterial = engine::content::kInvalidAssetId;
};

bool make_fixture(Fixture *fixture) noexcept {
  fixture->world.reset(new (std::nothrow) engine::runtime::World());
  fixture->assetDatabase.reset(new (std::nothrow)
                                   engine::renderer::AssetDatabase());
  fixture->meshRegistry.reset(new (std::nothrow)
                                  engine::renderer::GpuMeshRegistry());
  fixture->commandBuffer.reset(new (std::nothrow)
                                   engine::renderer::CommandBufferBuilder());
  fixture->prepContext.reset(new (std::nothrow)
                                 engine::runtime::RenderPrepPipelineContext());
  if ((fixture->world == nullptr) || (fixture->assetDatabase == nullptr) ||
      (fixture->meshRegistry == nullptr) ||
      (fixture->commandBuffer == nullptr) ||
      (fixture->prepContext == nullptr)) {
    return false;
  }
  engine::renderer::clear_asset_database(fixture->assetDatabase.get());
  engine::renderer::GpuMesh mesh{};
  mesh.vertexCount = 3U;
  const engine::renderer::MeshHandle meshHandle =
      engine::renderer::register_gpu_mesh(fixture->meshRegistry.get(), mesh);
  const char *meshPath = "integration://depth-order.mesh";
  fixture->meshAssetId = engine::content::make_asset_id_from_path(meshPath);
  if ((meshHandle == engine::renderer::kInvalidMeshHandle) ||
      !engine::renderer::register_mesh_asset(fixture->assetDatabase.get(),
                                             fixture->meshAssetId, meshPath,
                                             meshHandle)) {
    return false;
  }
  const char *opaquePath = "integration://depth-order-opaque.mat";
  const char *blendPath = "integration://depth-order-blend.mat";
  fixture->opaqueMaterial =
      engine::content::make_asset_id_from_path(opaquePath);
  fixture->blendMaterial = engine::content::make_asset_id_from_path(blendPath);
  engine::renderer::Material opaque{};
  engine::renderer::Material blend{};
  blend.opacity = 0.5F;
  return engine::renderer::register_material_asset(fixture->assetDatabase.get(),
                                                   fixture->opaqueMaterial,
                                                   opaquePath, opaque) &&
         engine::renderer::register_material_asset(fixture->assetDatabase.get(),
                                                   fixture->blendMaterial,
                                                   blendPath, blend);
}

/// Authors one draw at `position`; returns its entity index, or ~0 on
/// failure.
std::uint32_t add_draw(Fixture &fixture, const Vec3 &position,
                       bool transparent) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity =
      fixture.world->create_scene_object(transform);
  engine::runtime::MeshComponent component{};
  component.meshAssetId = fixture.meshAssetId;
  component.materialAssetId =
      transparent ? fixture.blendMaterial : fixture.opaqueMaterial;
  component.opacity = transparent ? 0.5F : 1.0F;
  if ((entity == engine::runtime::kInvalidEntity) ||
      !fixture.world->add_mesh_component(entity, component)) {
    return ~0U;
  }
  return entity.index;
}

/// A camera at the origin looking down -Z.
CameraState camera_down_negative_z(std::uint32_t projection, float farPlane) {
  CameraState camera{};
  camera.position = Vec3(0.0F, 0.0F, 0.0F);
  camera.target = Vec3(0.0F, 0.0F, -1.0F);
  camera.projection = projection;
  camera.nearPlane = 0.1F;
  camera.farPlane = farPlane;
  camera.orthographicSize = 10.0F;
  return camera;
}

/// Prepares `fixture` for `camera`; the first draw of `first` and of
/// `second` must arrive in that order, with distinct depth bits.
void check_order(Fixture &fixture, const CameraState &camera,
                 std::uint32_t first, std::uint32_t second,
                 const char *what) noexcept {
  const engine::runtime::RenderPrepView view =
      engine::runtime::make_render_prep_view(camera, 16.0F / 9.0F);
  if (!engine::tests::run_render_prep(
          fixture.world.get(), fixture.prepContext.get(),
          fixture.commandBuffer.get(), fixture.assetDatabase.get(),
          fixture.meshRegistry.get(), view)) {
    check(false, "render prep ran");
    return;
  }
  const engine::renderer::CommandBufferView draws =
      fixture.commandBuffer->view();
  std::size_t firstAt = draws.count;
  std::size_t secondAt = draws.count;
  for (std::size_t i = 0U; i < draws.count; ++i) {
    if (draws.data[i].entity == first) {
      firstAt = i;
    } else if (draws.data[i].entity == second) {
      secondAt = i;
    }
  }
  if ((firstAt == draws.count) || (secondAt == draws.count)) {
    check(false, "both draws reach the camera list");
    return;
  }
  const std::uint64_t firstDepth =
      draws.data[firstAt].sortKey.value & kDepthMask;
  const std::uint64_t secondDepth =
      draws.data[secondAt].sortKey.value & kDepthMask;
  std::printf("%s: depth bits %llu then %llu, positions %zu and %zu\n", what,
              static_cast<unsigned long long>(firstDepth),
              static_cast<unsigned long long>(secondDepth), firstAt, secondAt);
  check(firstDepth != secondDepth, what);
  check(firstAt < secondAt, what);
}

void check_orthographic_transparent() noexcept {
  Fixture fixture{};
  if (!make_fixture(&fixture)) {
    check(false, "orthographic fixture");
    return;
  }
  // Nearer created first, so entity order would draw it first.
  const std::uint32_t nearer =
      add_draw(fixture, Vec3(0.0F, 0.0F, -10.0F), true);
  const std::uint32_t farther =
      add_draw(fixture, Vec3(0.0F, 0.0F, -30.0F), true);
  check_order(
      fixture,
      camera_down_negative_z(CameraState::kProjectionOrthographic, 100.0F),
      farther, nearer, "an orthographic view draws transparents back to front");
}

void check_orthographic_opaque() noexcept {
  Fixture fixture{};
  if (!make_fixture(&fixture)) {
    check(false, "orthographic opaque fixture");
    return;
  }
  // Farther created first, so entity order would draw it first.
  const std::uint32_t farther =
      add_draw(fixture, Vec3(0.0F, 0.0F, -30.0F), false);
  const std::uint32_t nearer =
      add_draw(fixture, Vec3(0.0F, 0.0F, -10.0F), false);
  check_order(
      fixture,
      camera_down_negative_z(CameraState::kProjectionOrthographic, 100.0F),
      nearer, farther, "an orthographic view draws opaques front to back");
}

void check_far_perspective_transparent() noexcept {
  Fixture fixture{};
  if (!make_fixture(&fixture)) {
    check(false, "far perspective fixture");
    return;
  }
  // Both past 200 units, inside a far plane of 1000.
  const std::uint32_t nearer =
      add_draw(fixture, Vec3(0.0F, 0.0F, -300.0F), true);
  const std::uint32_t farther =
      add_draw(fixture, Vec3(0.0F, 0.0F, -700.0F), true);
  check_order(
      fixture,
      camera_down_negative_z(CameraState::kProjectionPerspective, 1000.0F),
      farther, nearer,
      "a perspective view with far 1000 orders transparents at 300 "
      "and 700 back to front");
}

void check_near_perspective_transparent() noexcept {
  Fixture fixture{};
  if (!make_fixture(&fixture)) {
    check(false, "near perspective fixture");
    return;
  }
  const std::uint32_t nearer = add_draw(fixture, Vec3(0.5F, 0.0F, -5.0F), true);
  const std::uint32_t farther =
      add_draw(fixture, Vec3(-0.5F, 0.0F, -6.0F), true);
  check_order(
      fixture,
      camera_down_negative_z(CameraState::kProjectionPerspective, 100.0F),
      farther, nearer,
      "a perspective view orders nearby transparents back to front");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_job_system(0U)) {
    return 1;
  }
  check_orthographic_transparent();
  check_orthographic_opaque();
  check_far_perspective_transparent();
  check_near_perspective_transparent();
  engine::core::shutdown_job_system();
  if (g_failures != 0) {
    std::fprintf(stderr, "render_prep_depth_order_test: %d failure(s)\n",
                 g_failures);
    return 10;
  }
  std::printf("render_prep_depth_order_test: every view orders its draws by "
              "depth\n");
  return 0;
}
