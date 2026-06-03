// Verifies instancing batch test behavior for the Engine test suite.

#include "engine/renderer/command_buffer.h"

#include <chrono>
#include <cstdio>

/// Runs this executable or test program.
int main() {
  static engine::renderer::CommandBufferBuilder builder;
  builder.reset();

  constexpr std::uint32_t kInstanceCount = 10000U;
  const auto begin = std::chrono::high_resolution_clock::now();
  for (std::uint32_t i = 0U; i < kInstanceCount; ++i) {
    engine::renderer::DrawCommand command{};
    command.sortKey.value = static_cast<std::uint64_t>(i);
    command.entity = i;
    command.mesh.id = 42U;
    command.material.albedo = engine::math::Vec3(0.2F, 0.6F, 0.9F);
    command.material.roughness = 0.35F;
    command.material.metallic = 0.1F;
    if (!builder.submit(command)) {
      std::printf("FAIL: submit stopped at %u\n", i);
      return 1;
    }
  }

  builder.sort_by_key();
  engine::renderer::StaticMeshBatch batch{};
  const std::size_t batchCount =
      engine::renderer::build_static_mesh_batches(builder.view(), 0U,
                                                  builder.command_count(),
                                                  &batch, 1U);
  const auto end = std::chrono::high_resolution_clock::now();
  const double elapsedMs =
      std::chrono::duration<double, std::milli>(end - begin).count();

  std::printf("[instancing_batch] commands=%u batches=%zu batch0=%u ms=%.4f\n",
              kInstanceCount, batchCount, batch.count, elapsedMs);

  if ((batchCount != 1U) || (batch.count != kInstanceCount)) {
    std::printf("FAIL: 10K identical meshes did not collapse to one batch\n");
    return 2;
  }

  return 0;
}
