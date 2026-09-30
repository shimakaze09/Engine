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

#include "../render_prep_harness.h"
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

  const engine::runtime::RenderPrepView view =
      engine::tests::active_camera_render_prep_view(16.0F / 9.0F);

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
    const bool prepared = engine::tests::run_render_prep(
        world.get(), prepContext.get(), commandBuffer.get(),
        assetDatabase.get(), meshRegistry.get(), view, kChunkSize);
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
