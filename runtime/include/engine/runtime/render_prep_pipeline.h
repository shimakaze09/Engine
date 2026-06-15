// Declares render prep pipeline types and APIs for the Engine runtime world.

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

/// Stores render prep chunk job data used by the engine.
struct RenderPrepChunkJobData final {
  const World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  std::size_t threadCount = 0U;
  renderer::CommandBufferBuilder *localBuffers = nullptr;
  renderer::AssetDatabase *assetDatabase = nullptr;
  const renderer::GpuMeshRegistry *meshRegistry = nullptr;
  std::atomic<bool> *frameGraphFailed = nullptr;
  math::Mat4 viewProjection{};
};

/// Stores merge commands job data used by the engine.
struct MergeCommandsJobData final {
  renderer::CommandBufferBuilder *merged = nullptr;
  renderer::CommandBufferBuilder *localBuffers = nullptr;
  std::size_t threadCount = 0U;
  std::atomic<bool> *frameGraphFailed = nullptr;
};

/// Stores render prep pipeline context data used by the engine.
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
    RenderPrepPipelineContext *context, const World *world,
    renderer::CommandBufferBuilder *mergedCommandBuffer,
    renderer::AssetDatabase *assetDatabase,
    const renderer::GpuMeshRegistry *meshRegistry,
    core::JobHandle renderPrepPhaseHandle, core::JobHandle renderPhaseHandle,
    std::atomic<bool> *frameGraphFailed, std::size_t frameThreadCount,
    std::size_t chunkSize, const math::Mat4 &viewProjection,
    core::JobHandle *outMergeHandle) noexcept;

} // namespace engine::runtime
