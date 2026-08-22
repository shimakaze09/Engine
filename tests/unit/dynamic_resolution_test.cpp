// Pins the dynamic-resolution controller contract (#138 v0.5 device
// reach): sustained over-budget frames step the factor down to the
// clamp, clear headroom recovers it, the hysteresis band holds steady,
// and the r_quality presets apply their cvar bundles exactly once per
// change.

#include "engine/core/cvar.h"
#include "engine/renderer/dynamic_resolution.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Feeds N identical frame samples through the controller.
float run_frames(engine::renderer::DynamicResolutionState &state,
                 float frameMs, float targetMs, float minScale, int frames) {
  float scale = state.scale;
  for (int i = 0; i < frames; ++i) {
    scale = engine::renderer::dynamic_resolution_step(state, frameMs,
                                                      targetMs, minScale);
  }
  return scale;
}

void test_overbudget_descends_and_clamps() {
  engine::renderer::DynamicResolutionState state{};
  // 33 ms frames against a 16.6 ms budget: far over even after
  // smoothing, so every cooldown expiry steps down until the clamp.
  const float scale = run_frames(state, 33.0F, 16.6F, 0.5F, 600);
  CHECK(std::fabs(scale - 0.5F) < 1e-6F,
        "sustained overshoot descends to the minimum clamp");
}

void test_headroom_recovers_to_full() {
  engine::renderer::DynamicResolutionState state{};
  state.scale = 0.5F;
  state.smoothedFrameMs = 33.0F; // stale overload reading to smooth away
  const float scale = run_frames(state, 5.0F, 16.6F, 0.5F, 600);
  CHECK(std::fabs(scale - 1.0F) < 1e-6F,
        "sustained headroom recovers to full scale");
}

void test_hysteresis_band_holds() {
  engine::renderer::DynamicResolutionState state{};
  state.scale = 0.8F;
  // Right on target: inside the band, no stepping either way.
  const float scale = run_frames(state, 16.6F, 16.6F, 0.5F, 600);
  CHECK(std::fabs(scale - 0.8F) < 1e-6F, "on-target frames hold the scale");
}

void test_invalid_samples_are_ignored() {
  engine::renderer::DynamicResolutionState state{};
  state.scale = 0.8F;
  CHECK(engine::renderer::dynamic_resolution_step(state, 0.0F, 16.6F,
                                                  0.5F) == 0.8F,
        "zero frame time is ignored");
  CHECK(engine::renderer::dynamic_resolution_step(state, 16.6F, 0.0F,
                                                  0.5F) == 0.8F,
        "zero target is ignored");
}

void test_quality_presets_apply() {
  using namespace engine;
  core::cvar_register_string("r_quality", "",
                             "Quality preset (test registration)");
  core::cvar_register_float("r_render_scale", 1.0F, "test");
  core::cvar_register_bool("r_shadows", true, "test");
  core::cvar_register_bool("r_ssao", true, "test");
  core::cvar_register_bool("r_bloom", true, "test");
  core::cvar_register_bool("r_fxaa", true, "test");

  static_cast<void>(core::cvar_set_string("r_quality", "low"));
  renderer::apply_quality_preset_if_changed();
  CHECK(std::fabs(core::cvar_get_float("r_render_scale", 0.0F) - 0.75F) <
            1e-6F,
        "low preset sets the render scale");
  CHECK(!core::cvar_get_bool("r_shadows", true), "low preset disables shadows");
  CHECK(!core::cvar_get_bool("r_ssao", true), "low preset disables SSAO");
  CHECK(core::cvar_get_bool("r_fxaa", false), "low preset keeps FXAA");

  // An unchanged value must not reapply over user tweaks.
  static_cast<void>(core::cvar_set_bool("r_shadows", true));
  renderer::apply_quality_preset_if_changed();
  CHECK(core::cvar_get_bool("r_shadows", false),
        "unchanged preset does not stomp later custom tweaks");

  static_cast<void>(core::cvar_set_string("r_quality", "high"));
  renderer::apply_quality_preset_if_changed();
  CHECK(core::cvar_get_bool("r_ssao", false), "high preset enables SSAO");
  CHECK(std::fabs(core::cvar_get_float("r_render_scale", 0.0F) - 1.0F) <
            1e-6F,
        "high preset restores full scale");

  // Unknown presets change nothing.
  static_cast<void>(core::cvar_set_bool("r_ssao", false));
  static_cast<void>(core::cvar_set_string("r_quality", "bogus"));
  renderer::apply_quality_preset_if_changed();
  CHECK(!core::cvar_get_bool("r_ssao", true), "unknown preset is a no-op");
}

void test_render_scale_clamps() {
  using namespace engine::renderer;
  set_render_scale(2.0F);
  CHECK(render_scale() == 1.0F, "scale clamps high to 1");
  set_render_scale(0.01F);
  CHECK(render_scale() == 0.25F, "scale clamps low to 0.25");
  set_render_scale(1.0F);
}

} // namespace

/// Runs this executable or test program.
int main() {
  test_overbudget_descends_and_clamps();
  test_headroom_recovers_to_full();
  test_hysteresis_band_holds();
  test_invalid_samples_are_ignored();
  test_quality_presets_apply();
  test_render_scale_clamps();

  if (g_failures != 0) {
    std::fprintf(stderr, "dynamic_resolution_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("dynamic_resolution_test: all checks passed\n");
  return 0;
}
