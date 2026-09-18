// Declares engine stats types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Frame-level engine counters shown by the stats overlay.
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
  /// Draws render prep could not fit into a command buffer last frame; a
  /// nonzero value means the frame was drawn incomplete (#519).
  std::uint32_t droppedDrawCommands = 0U;
  /// Point and spot lights submitted with last frame's draw list; both
  /// are collected from the same mutation epoch (#569).
  std::uint32_t sceneLights = 0U;
  /// Draw commands the main camera saw last frame, and the camera-culled
  /// commands render prep kept for the shadow passes (a sweep along the
  /// light reaches the view, or a casting local light's range) and for
  /// scene captures (a capture camera sees them) (#524).
  std::uint32_t drawCommands = 0U;
  std::uint32_t offscreenShadowCasters = 0U;
  std::uint32_t captureOnlyDraws = 0U;
  /// Fixed simulation steps last frame ran (0 while paused or stopped).
  std::uint32_t fixedSteps = 0U;
  /// Blend factor render prep used between the previous and current step
  /// poses (1 when the frame presented the current pose unblended).
  float interpolationAlpha = 1.0F;
};

/// Resets this object back to its reusable empty state for engine stats
/// (mutex-guarded; safe from any thread).
void reset_engine_stats() noexcept;
/// Publishes a whole-struct snapshot (mutex-guarded; safe from any thread).
void set_engine_stats(const EngineStats &stats) noexcept;
/// Consistent snapshot of the last published stats (mutex-guarded; a reader
/// never observes a torn mix of two publications).
EngineStats get_engine_stats() noexcept;

} // namespace engine::core
