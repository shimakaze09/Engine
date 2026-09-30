// Runs one render prep frame through the production pipeline for a test:
// the world's render-prep and render phases as jobs, the chunked prep
// jobs, the merge and sort, and the end of the frame, leaving the sorted
// draws in the caller's command buffer. Tests that pin what render prep
// emits share this one driver instead of each copying the frame graph.

#pragma once

#include "engine/core/job_system.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/world.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace engine::tests {

namespace render_prep_detail {

enum class WorldPhaseOp : std::uint8_t {
  BeginRenderPrep,
  BeginRender,
  EndFrame,
};

struct WorldPhaseJobData final {
  runtime::World *world = nullptr;
  WorldPhaseOp op = WorldPhaseOp::BeginRenderPrep;
};

inline void world_phase_job(void *userData) noexcept {
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

} // namespace render_prep_detail

/// The view the renderer's active camera gives at `aspect`, as a frame
/// builds it.
inline runtime::RenderPrepView
active_camera_render_prep_view(float aspect) noexcept {
  return runtime::make_render_prep_view(renderer::get_active_camera(), aspect);
}

/// Runs one render prep frame for `view` on the running job system, in
/// chunks of `chunkSize` entities. True when the frame graph ran, no job
/// failed and no draw was dropped for a full buffer.
inline bool run_render_prep(runtime::World *world,
                            runtime::RenderPrepPipelineContext *context,
                            renderer::CommandBufferBuilder *commandBuffer,
                            renderer::AssetDatabase *assetDatabase,
                            const renderer::GpuMeshRegistry *meshRegistry,
                            const runtime::RenderPrepView &view,
                            std::size_t chunkSize = 256U) noexcept {
  using render_prep_detail::world_phase_job;
  using render_prep_detail::WorldPhaseJobData;
  using render_prep_detail::WorldPhaseOp;
  if ((world == nullptr) || (context == nullptr) ||
      (commandBuffer == nullptr) || (assetDatabase == nullptr) ||
      (meshRegistry == nullptr) || !core::begin_frame_graph()) {
    return false;
  }

  std::atomic<bool> frameGraphFailed = false;

  WorldPhaseJobData prepPhaseData{world, WorldPhaseOp::BeginRenderPrep};
  core::Job prepPhaseJob{};
  prepPhaseJob.function = &world_phase_job;
  prepPhaseJob.data = &prepPhaseData;
  const core::JobHandle prepPhaseHandle = core::submit(prepPhaseJob);

  WorldPhaseJobData renderPhaseData{world, WorldPhaseOp::BeginRender};
  core::Job renderPhaseJob{};
  renderPhaseJob.function = &world_phase_job;
  renderPhaseJob.data = &renderPhaseData;
  const core::JobHandle renderPhaseHandle = core::submit(renderPhaseJob);

  if (!core::is_valid_handle(prepPhaseHandle) ||
      !core::is_valid_handle(renderPhaseHandle) ||
      !core::add_dependency(prepPhaseHandle, renderPhaseHandle)) {
    static_cast<void>(core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  core::JobHandle mergeHandle{};
  std::atomic<std::uint32_t> droppedDrawCommands{0U};
  if (!runtime::enqueue_render_prep_pipeline(
          context, world, commandBuffer, assetDatabase, meshRegistry,
          prepPhaseHandle, renderPhaseHandle, &frameGraphFailed,
          &droppedDrawCommands, static_cast<std::size_t>(core::thread_count()),
          chunkSize, view, 1.0F, &mergeHandle, nullptr, nullptr)) {
    static_cast<void>(core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  WorldPhaseJobData endFrameData{world, WorldPhaseOp::EndFrame};
  core::Job endFrameJob{};
  endFrameJob.function = &world_phase_job;
  endFrameJob.data = &endFrameData;
  const core::JobHandle endFrameHandle = core::submit(endFrameJob);
  if (!core::is_valid_handle(endFrameHandle) ||
      !core::add_dependency(mergeHandle, endFrameHandle)) {
    static_cast<void>(core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  core::wait_all();
  const bool jobsFailed = frameGraphFailed.load(std::memory_order_acquire);
  const bool ended = static_cast<bool>(core::end_frame_graph());
  return ended && !jobsFailed &&
         (droppedDrawCommands.load(std::memory_order_acquire) == 0U);
}

} // namespace engine::tests
