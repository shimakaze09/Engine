// Declares the dynamic-resolution controller and quality-preset entry
// points (#138 v0.5 device reach): a pure frame-time-driven scale
// stepper the runtime advances each frame, the renderer-owned effective
// render scale the flush sizes its scene passes by, and the r_quality
// preset bundles.

#pragma once

namespace engine::renderer {

/// Controller state for the frame-time-driven resolution stepper; one
/// instance lives for the run and persists smoothing across frames.
struct DynamicResolutionState final {
  /// Exponentially smoothed frame time; 0 until the first sample.
  float smoothedFrameMs = 0.0F;
  /// Current dynamic factor in [minScale, 1].
  float scale = 1.0F;
  /// Frames remaining before another step is allowed (hysteresis).
  int cooldownFrames = 0;
};

/// Advances the controller with one frame's wall time against the target
/// and returns the new dynamic factor in [minScale, 1]. Pure aside from
/// the state argument: steps down fast when the smoothed time overshoots
/// the target, recovers slowly when there is clear headroom, and holds
/// inside the hysteresis band.
float dynamic_resolution_step(DynamicResolutionState &state, float frameMs,
                              float targetMs, float minScale) noexcept;

/// Sets the effective render scale the next flush sizes scene passes by
/// (clamped to [0.25, 1]); the back buffer always presents at full size.
void set_render_scale(float scale) noexcept;

/// The effective render scale the flush is currently using.
float render_scale() noexcept;

/// Applies the r_quality preset bundle when the cvar changed since the
/// last call (low/medium/high set r_render_scale, r_shadows, r_ssao,
/// r_bloom, and r_fxaa together; unknown values log once and change
/// nothing). Called at flush start so console changes apply live.
void apply_quality_preset_if_changed() noexcept;

} // namespace engine::renderer
