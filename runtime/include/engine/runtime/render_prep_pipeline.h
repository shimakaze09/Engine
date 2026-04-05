#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "engine/core/job_system.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

struct RenderPrepChunkJobData final {
  const World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  std::size_t threadCount = 0U;
  renderer::CommandBufferBuilder *localBuffers = nullptr;
  const renderer::AssetDatabase *assetDatabase = nullptr;
  const renderer::GpuMeshRegistry *meshRegistry = nullptr;
  std::atomic<bool> *frameGraphFailed = nullptr;
};

struct MergeCommandsJobData final {
  renderer::CommandBufferBuilder *merged = nullptr;
  renderer::CommandBufferBuilder *localBuffers = nullptr;
  std::size_t threadCount = 0U;
  std::atomic<bool> *frameGraphFailed = nullptr;
};

struct RenderPrepPipelineContext final {
  static constexpr std::size_t kMaxFrameThreads = 16U;
  static constexpr std::size_t kMaxChunkJobs = 1024U;

  std::array<renderer::CommandBufferBuilder, kMaxFrameThreads>
      localCommandBuffers{};
  std::array<RenderPrepChunkJobData, kMaxChunkJobs> renderPrepJobData{};
  std::array<core::JobHandle, kMaxChunkJobs> renderPrepJobHandles{};
  MergeCommandsJobData mergeCommandsJobData{};
};

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
    core::JobHandle *outMergeHandle) noexcept;

} // namespace engine::runtime
