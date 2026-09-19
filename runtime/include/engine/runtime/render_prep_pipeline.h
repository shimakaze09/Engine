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

/// What the shadow and capture passes can see beyond the main camera:
/// a draw the camera frustum culls still enters the frame, in the
/// auxiliary list, when sweeping its bounds along the directional light
/// by `sweepDistance` reaches the camera frustum, when a shadow-casting
/// local light's range overlaps it, or when a capture camera sees it.
struct RenderPrepAuxiliaryInputs final {
  struct LocalCaster final {
    math::Vec3 position{};
    float radius = 0.0F;
  };
  bool directionalShadow = false;
  math::Vec3 lightDirection = math::Vec3(0.0F, -1.0F, 0.0F);
  float sweepDistance = 0.0F;
  std::size_t localCasterCount = 0U;
  std::array<LocalCaster, renderer::kMaxPointLights + renderer::kMaxSpotLights>
      localCasters{};
  std::size_t captureCount = 0U;
  std::array<math::Mat4, renderer::kMaxSceneCaptures> captureViewProjections{};
};

/// Inputs for one render-prep chunk job (world span -> local buffer).
struct RenderPrepChunkJobData final {
  const World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  std::size_t threadCount = 0U;
  renderer::CommandBufferBuilder *localBuffers = nullptr;
  renderer::AssetDatabase *assetDatabase = nullptr;
  const renderer::GpuMeshRegistry *meshRegistry = nullptr;
  std::atomic<bool> *frameGraphFailed = nullptr;
  /// Draws that did not fit a buffer this frame; a full buffer degrades
  /// the frame, it does not fail the graph.
  std::atomic<std::uint32_t> *droppedDrawCommands = nullptr;
  math::Mat4 viewProjection{};
  float interpolationAlpha = 1.0F;
  /// Null disables the auxiliary list (camera-culled draws are dropped).
  const RenderPrepAuxiliaryInputs *auxiliary = nullptr;
};

/// Inputs for the merge job combining per-thread buffers.
struct MergeCommandsJobData final {
  renderer::CommandBufferBuilder *merged = nullptr;
  /// Receives the commands without kPassCamera; null drops them.
  renderer::CommandBufferBuilder *mergedAuxiliary = nullptr;
  renderer::CommandBufferBuilder *localBuffers = nullptr;
  std::size_t threadCount = 0U;
  std::atomic<bool> *frameGraphFailed = nullptr;
  std::atomic<std::uint32_t> *droppedDrawCommands = nullptr;
};

/// Preallocated buffers and job bookkeeping for render prep.
struct RenderPrepPipelineContext final {
  static constexpr std::size_t kMaxFrameThreads = 16U;
  static constexpr std::size_t kMaxChunkJobs = 1024U;

  std::array<renderer::CommandBufferBuilder, kMaxFrameThreads>
      localCommandBuffers{};
  std::array<RenderPrepChunkJobData, kMaxChunkJobs> renderPrepJobData{};
  std::array<core::JobHandle, kMaxChunkJobs> renderPrepJobHandles{};
  MergeCommandsJobData mergeCommandsJobData{};
};

/// `mergedAuxiliaryBuffer` and `auxiliary` together enable the auxiliary
/// list of camera-culled draws for the shadow and capture passes;
/// either null keeps the frame camera-only.
bool enqueue_render_prep_pipeline(
    RenderPrepPipelineContext *context, const World *world,
    renderer::CommandBufferBuilder *mergedCommandBuffer,
    renderer::AssetDatabase *assetDatabase,
    const renderer::GpuMeshRegistry *meshRegistry,
    core::JobHandle renderPrepPhaseHandle, core::JobHandle renderPhaseHandle,
    std::atomic<bool> *frameGraphFailed,
    std::atomic<std::uint32_t> *droppedDrawCommands,
    std::size_t frameThreadCount, std::size_t chunkSize,
    const math::Mat4 &viewProjection, float interpolationAlpha,
    core::JobHandle *outMergeHandle,
    renderer::CommandBufferBuilder *mergedAuxiliaryBuffer = nullptr,
    const RenderPrepAuxiliaryInputs *auxiliary = nullptr) noexcept;

} // namespace engine::runtime
