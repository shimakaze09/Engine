// Declares engine stats types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Stores engine stats data used by the engine.
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

/// Resets this object back to its reusable empty state for engine stats.
void reset_engine_stats() noexcept;
/// Sets the requested value for engine stats.
void set_engine_stats(const EngineStats &stats) noexcept;
/// Returns the requested value for engine stats.
EngineStats get_engine_stats() noexcept;

} // namespace engine::core
