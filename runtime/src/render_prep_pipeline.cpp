#include "engine/runtime/render_prep_pipeline.h"

#include <atomic>
#include <cstddef>

#include "engine/core/logging.h"

namespace engine::runtime {

namespace {

void mark_graph_failed(std::atomic<bool> *frameGraphFailed) noexcept {
  if (frameGraphFailed != nullptr) {
    frameGraphFailed->store(true, std::memory_order_release);
  }
}

void render_prep_chunk_job(void *userData) noexcept {
  auto *jobData = static_cast<RenderPrepChunkJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)
      || (jobData->localBuffers == nullptr)
      || (jobData->assetDatabase == nullptr)
      || (jobData->meshRegistry == nullptr)) {
    return;
  }

  const std::size_t threadIndex =
      static_cast<std::size_t>(core::current_thread_index());
  const std::size_t threadCount = jobData->threadCount;
  if (threadIndex >= threadCount) {
    mark_graph_failed(jobData->frameGraphFailed);
    return;
  }

  const Entity *entities = nullptr;
  const WorldTransform *transforms = nullptr;
  if (!jobData->world->read_world_transform_range(
          jobData->startIndex, jobData->count, &entities, &transforms)) {
    mark_graph_failed(jobData->frameGraphFailed);
    return;
  }

  renderer::CommandBufferBuilder &localBuffer =
      jobData->localBuffers[threadIndex];

  for (std::size_t i = 0U; i < jobData->count; ++i) {
    const MeshComponent *meshComponent =
        jobData->world->get_mesh_component_ptr(entities[i]);
    if (meshComponent == nullptr) {
      continue;
    }

    const renderer::MeshHandle runtimeMesh = renderer::resolve_mesh_asset(
        jobData->assetDatabase, meshComponent->meshAssetId);
    if (runtimeMesh == renderer::kInvalidMeshHandle) {
      continue;
    }

    const renderer::GpuMesh *mesh =
        renderer::lookup_gpu_mesh(jobData->meshRegistry, runtimeMesh);
    if (mesh == nullptr) {
      continue;
    }

    renderer::DrawCommand command{};
    command.entity = entities[i].index;
    command.mesh = runtimeMesh;
    command.material = meshComponent->material;
    command.modelMatrix = transforms[i].matrix;

    if (!localBuffer.submit(command)) {
      core::log_message(core::LogLevel::Error,
                        "render_prep",
                        "command buffer full; entity dropped from frame");
      mark_graph_failed(jobData->frameGraphFailed);
      return;
    }
  }
}

void merge_command_buffers_job(void *userData) noexcept {
  auto *jobData = static_cast<MergeCommandsJobData *>(userData);
  if ((jobData == nullptr) || (jobData->merged == nullptr)
      || (jobData->localBuffers == nullptr)) {
    return;
  }

  jobData->merged->reset();
  for (std::size_t i = 0U; i < jobData->threadCount; ++i) {
    if (!jobData->merged->append_from(jobData->localBuffers[i])) {
      mark_graph_failed(jobData->frameGraphFailed);
      return;
    }
  }
  jobData->merged->sort_by_entity();
}

bool link_dependency(core::JobHandle prerequisite,
                     core::JobHandle dependent) noexcept {
  if (!core::is_valid_handle(prerequisite)
      || !core::is_valid_handle(dependent)) {
    return false;
  }

  return core::add_dependency(prerequisite, dependent);
}

} // namespace

bool enqueue_render_prep_pipeline(
    RenderPrepPipelineContext *context,
    const World *world,
    renderer::CommandBufferBuilder *mergedCommandBuffer,
    const renderer::AssetDatabase *assetDatabase,
    const renderer::GpuMeshRegistry *meshRegistry,
    core::JobHandle renderPrepPhaseHandle,
    core::JobHandle renderPhaseHandle,
    std::atomic<bool> *frameGraphFailed,
    std::size_t frameThreadCount,
    std::size_t chunkSize,
    core::JobHandle *outMergeHandle) noexcept {
  if ((context == nullptr) || (world == nullptr)
      || (mergedCommandBuffer == nullptr) || (assetDatabase == nullptr)
      || (meshRegistry == nullptr) || (chunkSize == 0U)
      || !core::is_valid_handle(renderPrepPhaseHandle)
      || !core::is_valid_handle(renderPhaseHandle)
      || (outMergeHandle == nullptr)) {
    return false;
  }

  *outMergeHandle = {};

  if ((frameThreadCount == 0U)
      || (frameThreadCount > context->localCommandBuffers.size())) {
    return false;
  }

  for (std::size_t i = 0U; i < frameThreadCount; ++i) {
    context->localCommandBuffers[i].reset();
  }

  const std::size_t transformCount = world->transform_count();
  if (transformCount > renderer::CommandBufferBuilder::kMaxDrawCommands) {
    return false;
  }

  std::size_t renderPrepJobCursor = 0U;
  std::size_t renderPrepHandleCount = 0U;

  for (std::size_t start = 0U; start < transformCount; start += chunkSize) {
    if ((renderPrepJobCursor >= context->renderPrepJobData.size())
        || (renderPrepHandleCount >= context->renderPrepJobHandles.size())) {
      return false;
    }

    const std::size_t count = ((start + chunkSize) > transformCount)
                                  ? (transformCount - start)
                                  : chunkSize;

    RenderPrepChunkJobData &prepData =
        context->renderPrepJobData[renderPrepJobCursor];
    prepData.world = world;
    prepData.startIndex = start;
    prepData.count = count;
    prepData.threadCount = frameThreadCount;
    prepData.localBuffers = context->localCommandBuffers.data();
    prepData.assetDatabase = assetDatabase;
    prepData.meshRegistry = meshRegistry;
    prepData.frameGraphFailed = frameGraphFailed;

    core::Job renderPrepJob{};
    renderPrepJob.function = &render_prep_chunk_job;
    renderPrepJob.data = &prepData;
    const core::JobHandle prepHandle = core::submit(renderPrepJob);
    if (!core::is_valid_handle(prepHandle)) {
      return false;
    }

    if (!link_dependency(renderPrepPhaseHandle, prepHandle)
        || !link_dependency(prepHandle, renderPhaseHandle)) {
      return false;
    }

    context->renderPrepJobHandles[renderPrepHandleCount] = prepHandle;
    ++renderPrepHandleCount;
    ++renderPrepJobCursor;
  }

  context->mergeCommandsJobData.merged = mergedCommandBuffer;
  context->mergeCommandsJobData.localBuffers =
      context->localCommandBuffers.data();
  context->mergeCommandsJobData.threadCount = frameThreadCount;
  context->mergeCommandsJobData.frameGraphFailed = frameGraphFailed;

  core::Job mergeJob{};
  mergeJob.function = &merge_command_buffers_job;
  mergeJob.data = &context->mergeCommandsJobData;
  const core::JobHandle mergeHandle = core::submit(mergeJob);
  if (!core::is_valid_handle(mergeHandle)) {
    return false;
  }

  if (!link_dependency(renderPhaseHandle, mergeHandle)) {
    return false;
  }

  *outMergeHandle = mergeHandle;
  return true;
}

} // namespace engine::runtime
