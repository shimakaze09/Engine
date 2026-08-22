// Implements the dynamic-resolution controller and quality presets
// (#138 v0.5 device reach): the frame-time stepper is pure state-in/
// state-out so its convergence, recovery, and hysteresis are unit
// tested; the effective scale and preset tracking are renderer-owned.

#include "engine/renderer/dynamic_resolution.h"

#include <algorithm>
#include <cstring>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"

namespace engine::renderer {

namespace {

// Smoothing and stepping constants: the EMA reacts within ~10 frames,
// steps move in 5% increments, and the cooldown keeps oscillation out
// of the steady state. Overshoot triggers earlier than recovery so the
// controller sheds load quickly and climbs back conservatively.
constexpr float kFrameTimeAlpha = 0.1F;
constexpr float kStep = 0.05F;
constexpr float kOvershootRatio = 1.15F;
constexpr float kRecoverRatio = 0.70F;
constexpr int kCooldownFrames = 30;

float g_renderScale = 1.0F;

/// One named preset: the cvar bundle the tier applies together.
struct QualityPreset final {
  const char *name;
  float renderScale;
  bool shadows;
  bool ssao;
  bool bloom;
  bool fxaa;
};

constexpr QualityPreset kPresets[] = {
    {"low", 0.75F, false, false, false, true},
    {"medium", 1.0F, true, false, true, true},
    {"high", 1.0F, true, true, true, true},
};

} // namespace

float dynamic_resolution_step(DynamicResolutionState &state, float frameMs,
                              float targetMs, float minScale) noexcept {
  if ((frameMs <= 0.0F) || (targetMs <= 0.0F)) {
    return state.scale;
  }
  const float clampedMin = std::clamp(minScale, 0.25F, 1.0F);
  state.smoothedFrameMs =
      (state.smoothedFrameMs <= 0.0F)
          ? frameMs
          : (state.smoothedFrameMs +
             kFrameTimeAlpha * (frameMs - state.smoothedFrameMs));
  if (state.cooldownFrames > 0) {
    --state.cooldownFrames;
    state.scale = std::clamp(state.scale, clampedMin, 1.0F);
    return state.scale;
  }
  if (state.smoothedFrameMs > targetMs * kOvershootRatio) {
    state.scale -= kStep;
    state.cooldownFrames = kCooldownFrames;
  } else if (state.smoothedFrameMs < targetMs * kRecoverRatio) {
    state.scale += kStep;
    state.cooldownFrames = kCooldownFrames;
  }
  state.scale = std::clamp(state.scale, clampedMin, 1.0F);
  return state.scale;
}

void set_render_scale(float scale) noexcept {
  g_renderScale = std::clamp(scale, 0.25F, 1.0F);
}

float render_scale() noexcept { return g_renderScale; }

void apply_quality_preset_if_changed() noexcept {
  static char lastApplied[16] = "";
  const char *quality = core::cvar_get_string("r_quality", "");
  if ((quality == nullptr) ||
      (std::strncmp(quality, lastApplied, sizeof(lastApplied)) == 0)) {
    return;
  }
  // Track the request before validating so an unknown value logs once,
  // not every frame.
  core::copy_string(lastApplied, sizeof(lastApplied), quality);
  if (quality[0] == '\0') {
    return; // "" = custom: user cvars stand as-is.
  }
  for (const QualityPreset &preset : kPresets) {
    if (std::strcmp(quality, preset.name) == 0) {
      static_cast<void>(
          core::cvar_set_float("r_render_scale", preset.renderScale));
      static_cast<void>(core::cvar_set_bool("r_shadows", preset.shadows));
      static_cast<void>(core::cvar_set_bool("r_ssao", preset.ssao));
      static_cast<void>(core::cvar_set_bool("r_bloom", preset.bloom));
      static_cast<void>(core::cvar_set_bool("r_fxaa", preset.fxaa));
      core::log_message(core::LogLevel::Info, "renderer",
                        "quality preset applied");
      return;
    }
  }
  core::log_message(core::LogLevel::Warning, "renderer",
                    "unknown r_quality preset (low/medium/high) — "
                    "cvars unchanged");
}

} // namespace engine::renderer
