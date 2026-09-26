// Pins the per-draw shading model through the production render prep: the
// key each draw carries names its own material's shading model, and the
// sort leaves one contiguous run per model inside each of the opaque and
// transparent halves. That grouping is what lets the flush bind one
// program per run rather than per draw, so a scene may mix a toon
// character, an unlit effect quad and a physically-lit prop in one frame.

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

/// Enumerates the world phase transitions the prep frame needs as jobs.
enum class WorldPhaseOp : std::uint8_t {
  BeginRenderPrep,
  BeginRender,
  EndFrame,
};

struct WorldPhaseJobData final {
  engine::runtime::World *world = nullptr;
  WorldPhaseOp op = WorldPhaseOp::BeginRenderPrep;
};

void world_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }
  switch (jobData->op) {
  case WorldPhaseOp::BeginRenderPrep:
    jobData->world->begin_render_prep_phase();
    break;
  case WorldPhaseOp::BeginRender:
    jobData->world->begin_render_phase();
    break;
  case WorldPhaseOp::EndFrame:
    jobData->world->end_frame_phase();
    break;
  }
}

/// Runs one render prep frame through the production pipeline, leaving the
/// sorted draws in `commandBuffer`.
bool run_render_prep(engine::runtime::World *world,
                     engine::runtime::RenderPrepPipelineContext *context,
                     engine::renderer::CommandBufferBuilder *commandBuffer,
                     engine::renderer::AssetDatabase *assetDatabase,
                     const engine::renderer::GpuMeshRegistry *meshRegistry,
                     const engine::math::Mat4 &viewProjection) noexcept {
  if (!engine::core::begin_frame_graph()) {
    return false;
  }

  std::atomic<bool> frameGraphFailed = false;

  WorldPhaseJobData prepPhaseData{world, WorldPhaseOp::BeginRenderPrep};
  engine::core::Job prepPhaseJob{};
  prepPhaseJob.function = &world_phase_job;
  prepPhaseJob.data = &prepPhaseData;
  const engine::core::JobHandle prepPhaseHandle =
      engine::core::submit(prepPhaseJob);

  WorldPhaseJobData renderPhaseData{world, WorldPhaseOp::BeginRender};
  engine::core::Job renderPhaseJob{};
  renderPhaseJob.function = &world_phase_job;
  renderPhaseJob.data = &renderPhaseData;
  const engine::core::JobHandle renderPhaseHandle =
      engine::core::submit(renderPhaseJob);

  if (!engine::core::is_valid_handle(prepPhaseHandle) ||
      !engine::core::is_valid_handle(renderPhaseHandle) ||
      !engine::core::add_dependency(prepPhaseHandle, renderPhaseHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  engine::core::JobHandle mergeHandle{};
  std::atomic<std::uint32_t> droppedDrawCommands{0U};
  if (!engine::runtime::enqueue_render_prep_pipeline(
          context, world, commandBuffer, assetDatabase, meshRegistry,
          prepPhaseHandle, renderPhaseHandle, &frameGraphFailed,
          &droppedDrawCommands,
          static_cast<std::size_t>(engine::core::thread_count()), 256U,
          viewProjection, 1.0F, &mergeHandle, nullptr, nullptr)) {
    static_cast<void>(engine::core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  WorldPhaseJobData endFrameData{world, WorldPhaseOp::EndFrame};
  engine::core::Job endFrameJob{};
  endFrameJob.function = &world_phase_job;
  endFrameJob.data = &endFrameData;
  const engine::core::JobHandle endFrameHandle =
      engine::core::submit(endFrameJob);
  if (!engine::core::is_valid_handle(endFrameHandle) ||
      !engine::core::add_dependency(mergeHandle, endFrameHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  engine::core::wait_all();
  const bool jobsFailed = frameGraphFailed.load(std::memory_order_acquire);
  const bool ended = static_cast<bool>(engine::core::end_frame_graph());
  return ended && !jobsFailed &&
         (droppedDrawCommands.load(std::memory_order_acquire) == 0U);
}

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

  const engine::renderer::CameraState camera =
      engine::renderer::get_active_camera();
  constexpr float kAspect = 16.0F / 9.0F;
  const engine::math::Mat4 viewProjection = engine::math::mul(
      engine::math::perspective(camera.fovRadians, kAspect, camera.nearPlane,
                                camera.farPlane),
      engine::math::look_at(camera.position, camera.target, camera.up));

  const bool prepared =
      run_render_prep(world.get(), prepContext.get(), commandBuffer.get(),
                      assetDatabase.get(), meshRegistry.get(), viewProjection);
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
