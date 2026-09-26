// The draw order render prep emits must not depend on how many worker
// threads produced it (#415 item 6).
//
// Render prep splits entities into chunks, each chunk fills whichever
// thread's local buffer ran it, and the merge concatenates those buffers
// in thread order before one sort. So which local buffer a draw lands in
// is a scheduling accident, and only a *total* order over the sort keys
// can undo it — a key comparison that leaves two draws equivalent lets
// the unstable sort keep them in whatever order the threads happened to
// produce. `draw_identity_less` is that total order, folding the owning
// entity and then the model matrix bit for bit.
//
// The code for it landed with no test. This is that test: the same scene
// prepped at 1, 2, 4 and 8 workers must emit the same sequence, compared
// as an exact fold over every key and every model matrix rather than as
// a count or a set, because a permutation preserves both of those.
//
// The scene is built to be hostile to a partial order: many draws share
// a mesh, a material and a depth, so their keys are equal and only the
// identity tiebreak separates them. A scene of distinct keys would sort
// identically under any comparison and prove nothing.

#include "engine/core/hash.h"
#include "engine/core/job_system.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/world.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

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
  auto *data = static_cast<WorldPhaseJobData *>(userData);
  if ((data == nullptr) || (data->world == nullptr)) {
    return;
  }
  switch (data->op) {
  case WorldPhaseOp::BeginRenderPrep:
    data->world->begin_render_prep_phase();
    break;
  case WorldPhaseOp::BeginRender:
    data->world->begin_render_phase();
    break;
  case WorldPhaseOp::EndFrame:
    data->world->end_frame_phase();
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
                     const engine::math::Mat4 &viewProjection,
                     std::size_t chunkSize) noexcept {
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
          static_cast<std::size_t>(engine::core::thread_count()), chunkSize,
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

/// Folds the emitted sequence: each draw's sort key, owning entity and
/// model matrix, in the order they were emitted. Order-sensitive by
/// construction — a permutation of the same draws gives a different
/// value, which a count or a sum would not.
std::uint64_t fold_draw_order(
    const engine::renderer::CommandBufferView &view) noexcept {
  std::uint64_t hash = engine::core::kFnv1a64Offset;
  const auto append_u64 = [&hash](std::uint64_t value) noexcept {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      hash = engine::core::fnv1a_64_append(
          hash, static_cast<std::uint8_t>(value >> shift));
    }
  };
  append_u64(view.count);
  for (std::uint32_t i = 0U; i < view.count; ++i) {
    const engine::renderer::DrawCommand &command = view.data[i];
    append_u64(command.sortKey.value);
    append_u64(static_cast<std::uint64_t>(command.entity));
    // The matrix bit for bit: two draws of one mesh at one depth differ
    // only here, so a fold that skipped it would miss a swap between
    // them — which is the swap this test exists to catch.
    std::array<std::uint32_t, 16> bits{};
    std::memcpy(bits.data(), &command.modelMatrix, sizeof(bits));
    for (const std::uint32_t word : bits) {
      append_u64(word);
    }
  }
  return hash;
}

/// How many draws share each (mesh, material, depth) so that only the
/// identity tiebreak can order them. Wide enough that render prep splits
/// them across chunks at every worker count under test.
constexpr std::size_t kDrawsPerRow = 24U;
constexpr std::size_t kRows = 6U;

/// Small enough that the authored draws span many chunks, so the work
/// genuinely spreads across the workers under test. With one chunk every
/// worker count would run it on one thread, the merge would see the same
/// input every time, and the comparison would hold for a reason that has
/// nothing to do with the ordering being total — measured: with the
/// tiebreak stubbed out and one chunk, all four counts still agreed.
constexpr std::size_t kChunkSize = 8U;
static_assert((kRows * kDrawsPerRow) / kChunkSize >= 8U,
              "the scene must span at least one chunk per worker under test");

} // namespace

/// Runs this executable or test program.
int main() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  std::unique_ptr<engine::renderer::AssetDatabase> assetDatabase(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> meshRegistry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  std::unique_ptr<engine::runtime::RenderPrepPipelineContext> prepContext(
      new (std::nothrow) engine::runtime::RenderPrepPipelineContext());
  if ((world == nullptr) || (assetDatabase == nullptr) ||
      (meshRegistry == nullptr) || (prepContext == nullptr)) {
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
  const char *meshPath = "integration://draw-order.mesh";
  const engine::content::AssetId meshAssetId =
      engine::content::make_asset_id_from_path(meshPath);
  if ((meshAssetId == engine::content::kInvalidAssetId) ||
      !engine::renderer::register_mesh_asset(assetDatabase.get(), meshAssetId,
                                             meshPath, meshHandle)) {
    return 3;
  }

  // Rows of identical draws: one mesh, one material, one depth per row,
  // so within a row every sort key is equal and only the entity and the
  // matrix separate them. Rows differ in depth so the key field is
  // exercised too.
  for (std::size_t row = 0U; row < kRows; ++row) {
    for (std::size_t i = 0U; i < kDrawsPerRow; ++i) {
      engine::runtime::Transform transform{};
      transform.position = engine::math::Vec3(
          static_cast<float>(i) * 0.25F, static_cast<float>(row) * 0.5F,
          -4.0F - static_cast<float>(row));
      const engine::runtime::Entity entity =
          world->create_scene_object(transform);
      engine::runtime::MeshComponent component{};
      component.meshAssetId = meshAssetId;
      if ((entity == engine::runtime::kInvalidEntity) ||
          !world->add_mesh_component(entity, component)) {
        return 4;
      }
    }
  }

  const engine::renderer::CameraState camera =
      engine::renderer::get_active_camera();
  constexpr float kAspect = 16.0F / 9.0F;
  const engine::math::Mat4 viewProjection = engine::math::mul(
      engine::math::perspective(camera.fovRadians, kAspect, camera.nearPlane,
                                camera.farPlane),
      engine::math::look_at(camera.position, camera.target, camera.up));

  constexpr std::array<std::uint32_t, 4> kWorkerCounts = {1U, 2U, 4U, 8U};
  std::uint64_t reference = 0U;
  std::uint32_t referenceDraws = 0U;
  std::uint32_t maxObservedWorkers = 0U;

  for (const std::uint32_t workers : kWorkerCounts) {
    if (!engine::core::initialize_job_system(workers)) {
      return 5;
    }
    const std::uint32_t observed = engine::core::worker_count();
    if (observed > maxObservedWorkers) {
      maxObservedWorkers = observed;
    }

    std::unique_ptr<engine::renderer::CommandBufferBuilder> commandBuffer(
        new (std::nothrow) engine::renderer::CommandBufferBuilder());
    if (commandBuffer == nullptr) {
      engine::core::shutdown_job_system();
      return 6;
    }
    const bool prepared =
        run_render_prep(world.get(), prepContext.get(), commandBuffer.get(),
                        assetDatabase.get(), meshRegistry.get(),
                        viewProjection, kChunkSize);
    if (!prepared) {
      engine::core::shutdown_job_system();
      std::fprintf(stderr, "FAIL: render prep failed at %u workers\n",
                   workers);
      return 7;
    }

    const engine::renderer::CommandBufferView view = commandBuffer->view();
    const std::uint64_t folded = fold_draw_order(view);
    std::printf("draw_order_thread_count_test: %u requested / %u actual "
                "workers, %u draws, order %llu\n",
                workers, observed, view.count,
                static_cast<unsigned long long>(folded));

    if (reference == 0U) {
      reference = folded;
      referenceDraws = view.count;
    } else if (folded != reference) {
      engine::core::shutdown_job_system();
      std::fprintf(stderr,
                   "FAIL: %u workers emitted a different draw order than the "
                   "first run (%llu against %llu)\n",
                   workers, static_cast<unsigned long long>(folded),
                   static_cast<unsigned long long>(reference));
      return 8;
    }
    engine::core::shutdown_job_system();
  }

  // Vacuity guards. A run that emitted nothing, or one where every
  // configuration collapsed to a single worker, would agree trivially.
  if (referenceDraws < static_cast<std::uint32_t>(kRows * kDrawsPerRow)) {
    std::fprintf(stderr,
                 "FAIL: only %u of %zu authored draws were emitted, so this "
                 "scene cannot show a reordering\n",
                 referenceDraws, kRows * kDrawsPerRow);
    return 20;
  }
  if (maxObservedWorkers < 2U) {
    std::printf("SKIPPED: this machine gave at most one worker, so no "
                "thread-count comparison happened\n");
    return 0;
  }

  std::printf("draw_order_thread_count_test: %u draws in one order across "
              "1/2/4/8 workers (up to %u actual)\n",
              referenceDraws, maxObservedWorkers);
  return 0;
}
