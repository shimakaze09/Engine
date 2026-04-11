#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

struct EngineStats final {
  float fps = 0.0F;
  float frameTimeMs = 0.0F;
  std::uint32_t drawCalls = 0U;
  std::uint64_t triCount = 0U;
  std::size_t entityCount = 0U;
  float memoryUsedMb = 0.0F;
  float gpuSceneMs = 0.0F;
  float gpuTonemapMs = 0.0F;
  float jobUtilizationPct = 0.0F;
};

void reset_engine_stats() noexcept;
void set_engine_stats(const EngineStats &stats) noexcept;
EngineStats get_engine_stats() noexcept;

} // namespace engine::core
