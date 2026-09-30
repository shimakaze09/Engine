// Pins the per-draw shading model through the production render prep: the
// key each draw carries names its own material's shading model, and the
// sort leaves one contiguous run per model inside each of the opaque and
// transparent halves. That grouping is what lets the flush bind one
// program per run rather than per draw, so a scene may mix a toon
// character, an unlit effect quad and a physically-lit prop in one frame.

#include "../render_prep_harness.h"
#include "engine/core/job_system.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/world.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using engine::renderer::ShadingModel;

/// One authored draw: where it sits, how it is lit, and whether it is
/// transparent.
struct Subject final {
  float x = 0.0F;
  ShadingModel model = ShadingModel::Pbr;
  float opacity = 1.0F;
};

/// Interleaved on purpose, and ordered so that submission order does not
/// already group the models: if the key's field did not outrank texture,
/// mesh and depth, the run check below would fail.
constexpr Subject kSubjects[] = {
    {-3.0F, ShadingModel::Toon, 1.0F},  {-2.0F, ShadingModel::Pbr, 1.0F},
    {-1.0F, ShadingModel::Unlit, 1.0F}, {0.0F, ShadingModel::Toon, 1.0F},
    {1.0F, ShadingModel::Pbr, 1.0F},    {2.0F, ShadingModel::Unlit, 0.5F},
    {3.0F, ShadingModel::Toon, 0.5F},   {4.0F, ShadingModel::Pbr, 0.5F},
};

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
  if (meshHandle == engine::renderer::kInvalidMeshHandle) {
    return 2;
  }
  const char *meshPath = "integration://shading-model.mesh";
  const engine::content::AssetId meshAssetId =
      engine::content::make_asset_id_from_path(meshPath);
  if ((meshAssetId == engine::content::kInvalidAssetId) ||
      !engine::renderer::register_mesh_asset(assetDatabase.get(), meshAssetId,
                                             meshPath, meshHandle)) {
    return 3;
  }

  // One registered material per model-and-opacity pair, which is what a
  // .mat file's shadingModel resolves to. The inline fallback on a mesh
  // component cannot name a model, by design: the model is the material's.
  const char *const kMaterialPaths[] = {
      "integration://shading-model-pbr.mat",
      "integration://shading-model-toon.mat",
      "integration://shading-model-unlit.mat",
      "integration://shading-model-pbr-blend.mat",
      "integration://shading-model-toon-blend.mat",
      "integration://shading-model-unlit-blend.mat",
  };
  const auto material_id_for = [&](ShadingModel model,
                                   float opacity) noexcept {
    const std::size_t index = static_cast<std::size_t>(model) +
                              ((opacity < 1.0F) ? 3U : 0U);
    return engine::content::make_asset_id_from_path(kMaterialPaths[index]);
  };
  for (std::size_t model = 0U; model < engine::renderer::kShadingModelCount;
       ++model) {
    for (int blend = 0; blend < 2; ++blend) {
      engine::renderer::Material params{};
      params.shadingModel = static_cast<ShadingModel>(model);
      params.opacity = (blend != 0) ? 0.5F : 1.0F;
      const std::size_t index =
          model + ((blend != 0) ? engine::renderer::kShadingModelCount : 0U);
      const engine::content::AssetId materialId =
          engine::content::make_asset_id_from_path(kMaterialPaths[index]);
      if ((materialId == engine::content::kInvalidAssetId) ||
          !engine::renderer::register_material_asset(
              assetDatabase.get(), materialId, kMaterialPaths[index], params)) {
        return 4;
      }
    }
  }

  for (const Subject &subject : kSubjects) {
    engine::runtime::Transform transform{};
    transform.position = engine::math::Vec3(subject.x, 0.0F, 0.0F);
    const engine::runtime::Entity entity =
        world->create_scene_object(transform);
    engine::runtime::MeshComponent component{};
    component.meshAssetId = meshAssetId;
    component.materialAssetId = material_id_for(subject.model,
                                                subject.opacity);
    component.opacity = subject.opacity;
    if ((entity == engine::runtime::kInvalidEntity) ||
        (component.materialAssetId == engine::content::kInvalidAssetId) ||
        !world->add_mesh_component(entity, component)) {
      return 5;
    }
  }

  if (!engine::core::initialize_job_system(0U)) {
    return 6;
  }

  const engine::runtime::RenderPrepView prepView =
      engine::tests::active_camera_render_prep_view(16.0F / 9.0F);

  const bool prepared = engine::tests::run_render_prep(
      world.get(), prepContext.get(), commandBuffer.get(), assetDatabase.get(),
      meshRegistry.get(), prepView);
  engine::core::shutdown_job_system();
  if (!prepared) {
    return 7;
  }

  const engine::renderer::CommandBufferView view = commandBuffer->view();
  if (view.count == 0U) {
    return 8;
  }

  // Every draw's key names its own material's shading model, and no run of
  // one model is ever re-entered: within each transparency half the models
  // appear in one contiguous block each.
  std::size_t seenRuns[engine::renderer::kShadingModelCount] = {};
  std::uint8_t previousModel = 0U;
  bool previousTransparent = false;
  bool first = true;
  for (std::uint32_t i = 0U; i < view.count; ++i) {
    const engine::renderer::DrawCommand &command = view.data[i];
    const std::uint8_t keyModel =
        engine::renderer::draw_key_shading_model(command.sortKey);
    if (!engine::renderer::shading_model_is_valid(keyModel)) {
      std::fprintf(stderr, "draw %u carries unknown shading model %u\n", i,
                   static_cast<unsigned int>(keyModel));
      return 9;
    }
    if (keyModel !=
        static_cast<std::uint8_t>(command.material.shadingModel)) {
      std::fprintf(stderr,
                   "draw %u key says model %u, material says %u\n", i,
                   static_cast<unsigned int>(keyModel),
                   static_cast<unsigned int>(command.material.shadingModel));
      return 10;
    }

    const bool transparent =
        engine::renderer::draw_key_is_transparent(command.sortKey);
    if (first || (transparent != previousTransparent) ||
        (keyModel != previousModel)) {
      if (transparent != previousTransparent) {
        for (std::size_t m = 0U; m < engine::renderer::kShadingModelCount;
             ++m) {
          seenRuns[m] = 0U;
        }
      }
      ++seenRuns[keyModel];
      if (seenRuns[keyModel] > 1U) {
        std::fprintf(stderr,
                     "shading model %u starts a second run at draw %u\n",
                     static_cast<unsigned int>(keyModel), i);
        return 11;
      }
    }
    first = false;
    previousModel = keyModel;
    previousTransparent = transparent;
  }

  std::printf("render_prep_shading_model_test: %u draws in model-grouped "
              "runs\n",
              view.count);
  return 0;
}
