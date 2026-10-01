// Pins which draws render prep puts in the transparent half of the draw
// list, through the production pipeline. A material's Alpha Mode is the
// author's choice of how it composites, so Blend must blend even at
// opacity 1: it used to be saved and offered in the editor while the
// classification read only opacity, so a soft-edged Blend card drew fully
// opaque. Mask cuts out and stays opaque; an opacity below 1 still makes
// any material transparent.

#include "../render_prep_harness.h"
#include "engine/core/job_system.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/material.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/world.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using engine::renderer::AlphaMode;

/// One authored material and whether its draw must blend.
struct Case final {
  const char *path = "";
  AlphaMode mode = AlphaMode::Opaque;
  float opacity = 1.0F;
  bool transparent = false;
  const char *what = "";
};

constexpr std::array<Case, 5> kCases = {{
    {"integration://alpha-opaque.mat", AlphaMode::Opaque, 1.0F, false,
     "an Opaque material at opacity 1 is opaque"},
    {"integration://alpha-mask.mat", AlphaMode::Mask, 1.0F, false,
     "a Mask material cuts out and stays opaque"},
    {"integration://alpha-blend.mat", AlphaMode::Blend, 1.0F, true,
     "a Blend material at opacity 1 blends"},
    {"integration://alpha-blend-half.mat", AlphaMode::Blend, 0.5F, true,
     "a Blend material at opacity 0.5 blends"},
    {"integration://alpha-opaque-half.mat", AlphaMode::Opaque, 0.5F, true,
     "an opacity below 1 blends whatever the mode"},
}};

} // namespace

/// Runs this executable or test program.
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
  engine::renderer::GpuMesh mesh{};
  mesh.vertexCount = 3U;
  const engine::renderer::MeshHandle meshHandle =
      engine::renderer::register_gpu_mesh(meshRegistry.get(), mesh);
  const char *meshPath = "integration://alpha-mode.mesh";
  const engine::content::AssetId meshAssetId =
      engine::content::make_asset_id_from_path(meshPath);
  if ((meshHandle == engine::renderer::kInvalidMeshHandle) ||
      !engine::renderer::register_mesh_asset(assetDatabase.get(), meshAssetId,
                                             meshPath, meshHandle)) {
    return 2;
  }

  std::array<std::uint32_t, kCases.size()> entities{};
  for (std::size_t i = 0U; i < kCases.size(); ++i) {
    const Case &row = kCases[i];
    engine::renderer::Material material{};
    material.alphaMode = row.mode;
    material.opacity = row.opacity;
    const engine::content::AssetId materialId =
        engine::content::make_asset_id_from_path(row.path);
    if (!engine::renderer::register_material_asset(
            assetDatabase.get(), materialId, row.path, material)) {
      return 3;
    }
    engine::runtime::Transform transform{};
    transform.position =
        engine::math::Vec3(static_cast<float>(i) - 2.0F, 0.0F, -5.0F);
    const engine::runtime::Entity entity =
        world->create_scene_object(transform);
    engine::runtime::MeshComponent component{};
    component.meshAssetId = meshAssetId;
    component.materialAssetId = materialId;
    component.opacity = row.opacity;
    if ((entity == engine::runtime::kInvalidEntity) ||
        !world->add_mesh_component(entity, component)) {
      return 4;
    }
    entities[i] = entity.index;
  }

  if (!engine::core::initialize_job_system(0U)) {
    return 5;
  }
  engine::renderer::CameraState camera{};
  camera.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  camera.target = engine::math::Vec3(0.0F, 0.0F, -1.0F);
  const bool prepared = engine::tests::run_render_prep(
      world.get(), prepContext.get(), commandBuffer.get(), assetDatabase.get(),
      meshRegistry.get(),
      engine::runtime::make_render_prep_view(camera, 16.0F / 9.0F));
  engine::core::shutdown_job_system();
  if (!prepared) {
    return 6;
  }

  int failures = 0;
  const engine::renderer::CommandBufferView draws = commandBuffer->view();
  for (std::size_t i = 0U; i < kCases.size(); ++i) {
    bool found = false;
    bool transparent = false;
    for (std::size_t d = 0U; d < draws.count; ++d) {
      if (draws.data[d].entity == entities[i]) {
        found = true;
        transparent = engine::renderer::draw_key_is_transparent(
            draws.data[d].sortKey);
      }
    }
    if (!found || (transparent != kCases[i].transparent)) {
      std::fprintf(stderr, "FAIL: %s (%s)\n", kCases[i].what,
                   found ? (transparent ? "transparent" : "opaque")
                         : "not drawn");
      ++failures;
    }
  }
  if (failures != 0) {
    return 10;
  }
  std::printf("render_prep_alpha_mode_test: every alpha mode lands in its "
              "half of the draw list\n");
  return 0;
}
