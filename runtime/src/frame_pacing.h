// Declares frame pacing helpers for the engine pipeline: vsync interval
// normalization, the pure frame-cap wait computation (unit-tested exactly;
// the wall-clock wait itself stays in the pipeline), and the hybrid
// sleep-then-spin wait.

#pragma once

namespace engine::runtime {

/// Clamps a vsync cvar value to a supported present interval:
/// negative -> -1 (adaptive), 0 -> off, anything else -> 1.
int normalize_vsync_interval(int requested) noexcept;

/// Seconds left to wait so the frame spans 1/maxFps seconds; 0 when
/// uncapped (maxFps <= 0) or the frame already ran past its budget.
double frame_cap_wait_seconds(double elapsedSeconds, int maxFps) noexcept;

/// Blocks for waitSeconds using a coarse sleep followed by a spin so the
/// cap stays precise despite OS timer granularity. No-op for waits <= 0.
void wait_for_frame_cap(double waitSeconds) noexcept;

} // namespace engine::runtime
