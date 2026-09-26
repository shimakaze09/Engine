// Verifies the production post chain (flush_post_chain) against a fake
// render device: bloom/luminance mip-chain creation is transactional, a
// failed chain disables the pass instead of rendering into the default
// framebuffer, failures retry on resize rather than every frame, and a
// healthy chain is created exactly once per size (audit N-10). Auto
// exposure adapts on the device: each frame's adaptation pass reads last
// frame's exposure target and writes the other, with the blend step the
// elapsed time and r_auto_exposure_speed give, tonemap samples the one just
// written, r_exposure applies as the compensation, and switching auto
// exposure off restores the manual exposure and restarts the adaptation.

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "command_buffer_post_resources.h"
#include "engine/core/cvar.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"

#include "../fake_render_device.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace engine::renderer {

namespace {

// The RGBA16F post-chain allocations are the ones the audit N-10 tests
// script failures for; other formats always succeed.
int g_hdrTextureCreates = 0;
bool g_failHdrTextureCreate = false;

DeviceTextureHandle fake_create_texture(const TextureDesc &desc) noexcept {
  if (desc.format == TextureFormat::RGBA16F) {
    ++g_hdrTextureCreates;
    if (g_failHdrTextureCreate) {
      return kInvalidDeviceTexture;
    }
  }
  return tests::fake::create_texture(desc);
}

RenderTargetHandle fake_create_render_target(
    const RenderTargetDesc &desc) noexcept {
  const RenderTargetHandle target = tests::fake::create_render_target(desc);
  // The contract rejects targets over failed (invalid) textures; mirroring
  // that here keeps "no target is created over a failed texture" honest.
  if ((target.value != 0U) && (desc.colorCount > 0U) &&
      (desc.colors[0].texture == kInvalidDeviceTexture)) {
    tests::fake::destroy_render_target(target);
    return RenderTargetHandle{};
  }
  return target;
}

/// The fake parameter slots the harness resolves the adaptation and
/// tonemap uniforms to, and what the flush last uploaded to each.
constexpr ShaderParam kAdaptLuminance{20};
constexpr ShaderParam kAdaptPrevious{21};
constexpr ShaderParam kAdaptParams{22};
constexpr ShaderParam kTonemapExposure{30};
constexpr ShaderParam kTonemapExposureTexture{31};
constexpr ShaderParam kTonemapAuto{32};
float g_adaptParams[4] = {};
float g_tonemapExposure = -1.0F;
int g_tonemapAuto = -1;
/// Texture on each of the first three units, the target bound, and per
/// draw the target it landed on with units 1 and 2 at the time.
std::uint32_t g_units[3] = {};
std::uint32_t g_target = 0U;
struct DrawRecord final {
  std::uint32_t target = 0U;
  std::uint32_t unit0 = 0U;
  std::uint32_t unit1 = 0U;
  std::uint32_t unit2 = 0U;
};
constexpr std::size_t kMaxDraws = 64U;
DrawRecord g_drawLog[kMaxDraws] = {};
std::size_t g_drawCount = 0U;

void record_set_param_vec4(ShaderParam param, const float *value) noexcept {
  if (param == kAdaptParams) {
    for (int i = 0; i < 4; ++i) {
      g_adaptParams[i] = value[i];
    }
  }
}
void record_set_param_f32(ShaderParam param, float value) noexcept {
  if (param == kTonemapExposure) {
    g_tonemapExposure = value;
  }
}
void record_set_param_i32(ShaderParam param, std::int32_t value) noexcept {
  if (param == kTonemapAuto) {
    g_tonemapAuto = value;
  }
}
void record_bind_texture_slot(std::uint32_t slot,
                              DeviceTextureHandle texture) noexcept {
  if (slot < 3U) {
    g_units[slot] = texture.value;
  }
}
void record_bind_render_target(RenderTargetHandle target) noexcept {
  g_target = target.value;
  tests::fake::bind_render_target(target);
}
void record_draw(DeviceGeometryHandle geometry, PrimitiveTopology topology,
                 std::int32_t first, std::int32_t count) noexcept {
  if (g_drawCount < kMaxDraws) {
    g_drawLog[g_drawCount] =
        DrawRecord{g_target, g_units[0], g_units[1], g_units[2]};
  }
  ++g_drawCount;
  tests::fake::draw(geometry, topology, first, count);
}

/// Installs the fake device table and clears its record.
void reset_fake_device() noexcept {
  g_hdrTextureCreates = 0;
  g_failHdrTextureCreate = false;
  tests::reset_fake_device();
  RenderDevice &device = tests::fake_device();
  device.create_texture = &fake_create_texture;
  device.destroy_texture = &tests::fake::destroy_texture;
  device.create_render_target = &fake_create_render_target;
  device.destroy_render_target = &tests::fake::destroy_render_target;
  device.bind_render_target = &record_bind_render_target;
  device.draw = &record_draw;
  device.bind_program = &tests::fake::bind_program;
  device.bind_texture_slot = &record_bind_texture_slot;
  device.set_param_i32 = &record_set_param_i32;
  device.set_param_f32 = &record_set_param_f32;
  device.set_param_vec2 = &tests::fake::set_param_vec2;
  device.set_param_vec4 = &record_set_param_vec4;
  device.set_viewport = &tests::fake::set_viewport;
  device.apply_render_state = &tests::fake::apply_render_state;
  device.clear = &tests::fake::clear;
}

} // namespace

void gpu_profiler_begin_pass(GpuPassId) noexcept {}
void gpu_profiler_end_pass(GpuPassId) noexcept {}

} // namespace engine::renderer

namespace {

using namespace engine::renderer;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Number of bloom+luminance HDR texture / render-target create calls
/// issued since the given baselines.
int chain_create_calls_since(int hdrBaseline, int targetBaseline) noexcept {
  return (g_hdrTextureCreates - hdrBaseline) +
         (engine::tests::fake_creates(engine::tests::FakeKind::RenderTarget) -
          targetBaseline);
}

/// Returns true when every bloom mip texture and render-target slot is
/// invalid.
bool bloom_chain_is_zeroed(const BackendState &backend) noexcept {
  for (int i = 0; i < BackendState::kBloomMipLevels; ++i) {
    if ((backend.bloomMipTextures[i] != kInvalidDeviceTexture) ||
        (backend.bloomMipTargets[i].value != 0U)) {
      return false;
    }
  }
  return true;
}

/// Returns true when every luminance mip and exposure texture and
/// render-target slot is invalid.
bool luminance_chain_is_zeroed(const BackendState &backend) noexcept {
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if ((backend.lumMipTextures[i] != kInvalidDeviceTexture) ||
        (backend.lumMipTargets[i].value != 0U)) {
      return false;
    }
  }
  for (int i = 0; i < 2; ++i) {
    if ((backend.exposureTextures[i] != kInvalidDeviceTexture) ||
        (backend.exposureTargets[i].value != 0U)) {
      return false;
    }
  }
  return true;
}

/// Builds a fresh flush context over the shared backend and current pass
/// resources at the given drawable size.
FrameFlushContext make_context(const SceneLightData &lights, int width,
                               int height, float timeSeconds = 0.0F) noexcept {
  return FrameFlushContext{.backend = backend_state(),
                           .dev = render_device(),
                           .commandBufferView = {},
                           .registry = nullptr,
                           .lights = lights,
                           .timeSeconds = timeSeconds,
                           .passRes = get_pass_resources(),
                           .drawableWidth = width,
                           .drawableHeight = height,
                           .fogSettings = {},
                           .heightFogSettings = {},
                           .envSkyboxTexture = {},
                           .iblPrefilteredTex = {},
                           .iblIrradianceTex = {},
                           .iblAvailable = false,
                           .viewMat = {},
                           .projMat = {},
                           .viewProjection = {},
                           .nearP = 0.1F,
                           .farP = 100.0F,
                           .opaqueCount = 0U,
                           .totalCount = 0U,
                           .opaqueBatchCount = 0U,
                           .gbufferDebugMode = 0};
}

/// Resets device, pass resources, and backend program state so flush_post_chain
/// runs its bloom, auto-exposure, and tonemap passes.
bool reset_post_chain_harness() noexcept {
  shutdown_pass_resources();
  reset_fake_device();
  BackendState &backend = backend_state();
  backend = BackendState{};
  backend.bloomThresholdProgram = DeviceProgramHandle{1U};
  backend.bloomDownsampleProgram = DeviceProgramHandle{2U};
  backend.bloomUpsampleProgram = DeviceProgramHandle{3U};
  backend.tonemapProgram = DeviceProgramHandle{4U};
  backend.luminanceProgram = DeviceProgramHandle{5U};
  backend.exposureAdaptProgram = DeviceProgramHandle{7U};
  backend.adaptLuminanceLoc = kAdaptLuminance;
  backend.adaptPreviousLoc = kAdaptPrevious;
  backend.adaptParamsLoc = kAdaptParams;
  backend.tonemapExposureLocation = kTonemapExposure;
  backend.tonemapExposureTextureLoc = kTonemapExposureTexture;
  backend.tonemapAutoExposureLoc = kTonemapAuto;
  backend.autoExposureAvailable = true;
  backend.emptyGeometry = DeviceGeometryHandle{6U};
  return initialize_pass_resources(640, 480);
}

/// EXPECTATION (audit N-10): when framebuffer creation fails, bloom and
/// auto exposure stay unavailable, the partial chains are released, no post
/// pass draws into the default framebuffer, the failed size is not retried
/// every frame, and a later resize retries and recovers.
void test_framebuffer_failure_disables_post_chains() noexcept {
  CHECK(reset_post_chain_harness(), "pass resources initialize");
  static const SceneLightData lights{};
  BackendState &backend = backend_state();

  engine::tests::fake_log().failKinds =
      engine::tests::fake_kind_bit(engine::tests::FakeKind::RenderTarget);
  const int aliveTextures =
      engine::tests::fake_alive(engine::tests::FakeKind::Texture);
  const int aliveRenderTargets =
      engine::tests::fake_alive(engine::tests::FakeKind::RenderTarget);
  FrameFlushContext ctx = make_context(lights, 640, 480);
  flush_post_chain(ctx);

  CHECK(engine::tests::fake_log().drawsToBackBuffer == 0,
        "no pass draws into the back buffer on chain failure");
  CHECK(bloom_chain_is_zeroed(backend), "failed bloom chain fully released");
  CHECK(luminance_chain_is_zeroed(backend),
        "failed luminance chain fully released");
  CHECK(engine::tests::fake_alive(engine::tests::FakeKind::Texture) ==
            aliveTextures,
        "chain failure leaks no textures");
  CHECK(engine::tests::fake_alive(engine::tests::FakeKind::RenderTarget) ==
            aliveRenderTargets,
        "chain failure leaks no render targets");
  CHECK(engine::tests::fake_log().draws > 0,
        "tonemap still runs into the final target");

  const int hdrBaseline = g_hdrTextureCreates;
  const int targetBaseline =
      engine::tests::fake_creates(engine::tests::FakeKind::RenderTarget);
  FrameFlushContext repeatCtx = make_context(lights, 640, 480);
  flush_post_chain(repeatCtx);
  CHECK(chain_create_calls_since(hdrBaseline, targetBaseline) == 0,
        "failed size is not retried every frame");
  CHECK(engine::tests::fake_log().drawsToBackBuffer == 0,
        "repeat frame still never draws into the back buffer");

  engine::tests::fake_log().failKinds = 0U;
  FrameFlushContext resizedCtx = make_context(lights, 800, 600);
  flush_post_chain(resizedCtx);
  CHECK(!bloom_chain_is_zeroed(backend), "resize retries the bloom chain");
  CHECK(!luminance_chain_is_zeroed(backend),
        "resize retries the luminance chain");
  CHECK(engine::tests::fake_log().drawsToBackBuffer == 0,
        "recovered chain draws only into offscreen targets");
}

/// EXPECTATION (audit N-10): a texture-create failure is handled the same
/// way and never hands an invalid texture to create_render_target (which
/// would build a target the completeness gate must reject).
void test_texture_failure_disables_post_chains() noexcept {
  CHECK(reset_post_chain_harness(), "pass resources initialize");
  static const SceneLightData lights{};
  BackendState &backend = backend_state();

  g_failHdrTextureCreate = true;
  const int targetBaseline =
      engine::tests::fake_creates(engine::tests::FakeKind::RenderTarget);
  FrameFlushContext ctx = make_context(lights, 640, 480);
  flush_post_chain(ctx);

  CHECK(engine::tests::fake_log().drawsToBackBuffer == 0,
        "no pass draws into the back buffer on texture failure");
  CHECK(bloom_chain_is_zeroed(backend), "failed bloom chain fully released");
  CHECK(luminance_chain_is_zeroed(backend),
        "failed luminance chain fully released");
  CHECK(engine::tests::fake_creates(engine::tests::FakeKind::RenderTarget) ==
            targetBaseline,
        "no render target is created over a failed texture");

  const int hdrBaseline = g_hdrTextureCreates;
  FrameFlushContext repeatCtx = make_context(lights, 640, 480);
  flush_post_chain(repeatCtx);
  CHECK(chain_create_calls_since(hdrBaseline, targetBaseline) == 0,
        "failed size is not retried every frame");
}

/// EXPECTATION: a healthy device creates each chain exactly once per size,
/// the bloom passes draw offscreen only, and a repeat frame reuses the
/// existing chains without further create calls.
void test_success_creates_chains_once_per_size() noexcept {
  CHECK(reset_post_chain_harness(), "pass resources initialize");
  static const SceneLightData lights{};
  BackendState &backend = backend_state();

  FrameFlushContext ctx = make_context(lights, 640, 480);
  flush_post_chain(ctx);
  CHECK(!bloom_chain_is_zeroed(backend), "bloom chain created");
  CHECK(!luminance_chain_is_zeroed(backend), "luminance chain created");
  CHECK(engine::tests::fake_log().draws > 0, "post chain draws");
  CHECK(engine::tests::fake_log().drawsToBackBuffer == 0,
        "all post draws land in offscreen targets");

  const int hdrBaseline = g_hdrTextureCreates;
  const int targetBaseline =
      engine::tests::fake_creates(engine::tests::FakeKind::RenderTarget);
  FrameFlushContext repeatCtx = make_context(lights, 640, 480);
  flush_post_chain(repeatCtx);
  CHECK(chain_create_calls_since(hdrBaseline, targetBaseline) == 0,
        "same-size repeat frame creates nothing");
}

/// The one draw of a flush that landed on `target`, or nullptr.
const DrawRecord *draw_on(std::uint32_t target) noexcept {
  const std::size_t count = (g_drawCount < kMaxDraws) ? g_drawCount : kMaxDraws;
  for (std::size_t i = 0U; i < count; ++i) {
    if (g_drawLog[i].target == target) {
      return &g_drawLog[i];
    }
  }
  return nullptr;
}

/// Runs one frame at `timeSeconds` and returns whether the adaptation pass
/// drew; the draw log holds that frame alone.
bool adaptation_frame(float timeSeconds) noexcept {
  static const SceneLightData lights{};
  g_drawCount = 0U;
  g_tonemapExposure = -1.0F;
  g_tonemapAuto = -1;
  FrameFlushContext ctx = make_context(lights, 640, 480, timeSeconds);
  flush_post_chain(ctx);
  const BackendState &backend = backend_state();
  return (draw_on(backend.exposureTargets[0].value) != nullptr) ||
         (draw_on(backend.exposureTargets[1].value) != nullptr);
}

/// EXPECTATION (#541): with auto exposure on, each frame's adaptation pass
/// averages the last luminance mip, reads last frame's exposure and writes
/// the other 1x1 target with the blend step 1 - exp(-speed * dt); the first
/// frame, with nothing to blend from, snaps to its target. Tonemap samples
/// the target just written and applies r_exposure on top. On base the
/// adaptation set the exposure to itself, nothing read the chain, and
/// r_exposure was ignored.
void test_auto_exposure_adapts_on_the_device() noexcept {
  CHECK(reset_post_chain_harness(), "pass resources initialize");
  BackendState &backend = backend_state();
  engine::core::cvar_set_bool("r_auto_exposure", true);
  engine::core::cvar_set_float("r_auto_exposure_speed", 2.0F);
  engine::core::cvar_set_float("r_exposure", 1.5F);

  CHECK(adaptation_frame(10.0F), "the first frame adapts");
  const std::uint32_t lastMip =
      backend.lumMipTextures[BackendState::kLuminanceMipLevels - 1].value;
  const DrawRecord *first = draw_on(backend.exposureTargets[1].value);
  CHECK((first != nullptr) && (first->unit0 == lastMip) &&
            (first->unit1 == backend.exposureTextures[0].value),
        "it averages the last mip and reads the other target");
  CHECK(g_adaptParams[3] == 0.0F,
        "with nothing adapted yet it snaps to the target");
  CHECK((g_adaptParams[1] == 0.1F) && (g_adaptParams[2] == 10.0F),
        "the exposure range comes from the min and max cvars");
  const DrawRecord *tonemap =
      draw_on(pass_resource_target(get_pass_resources().finalColor).value);
  CHECK((tonemap != nullptr) &&
            (tonemap->unit2 == backend.exposureTextures[1].value),
        "tonemap samples the exposure just written");
  CHECK(g_tonemapAuto == 1, "tonemap is told to apply it");
  CHECK(g_tonemapExposure == 1.5F, "r_exposure is the compensation");

  CHECK(adaptation_frame(10.1F), "the next frame adapts again");
  const DrawRecord *second = draw_on(backend.exposureTargets[0].value);
  CHECK((second != nullptr) &&
            (second->unit1 == backend.exposureTextures[1].value),
        "it reads last frame's exposure and writes the other target");
  CHECK(g_adaptParams[3] == 1.0F, "and blends from it");
  // Same single-precision inputs as the flush; the bound covers a libm
  // whose expf is not correctly rounded (a few ulp of a value near 0.2).
  constexpr float kExpUlps = 1e-6F;
  const float expected = 1.0F - std::exp(-2.0F * (10.1F - 10.0F));
  CHECK(std::fabs(g_adaptParams[0] - expected) <= kExpUlps,
        "the blend step is 1 - exp(-speed * elapsed)");

  CHECK(adaptation_frame(20.0F), "a long stall still adapts");
  CHECK(std::fabs(g_adaptParams[0] - (1.0F - std::exp(-2.0F * 0.25F))) <=
            kExpUlps,
        "and its step is capped at a quarter second");

  engine::core::cvar_set_bool("r_auto_exposure", false);
  CHECK(!adaptation_frame(20.1F), "manual exposure runs no adaptation");
  CHECK((g_tonemapAuto == 0) && (g_tonemapExposure == 1.5F),
        "tonemap applies r_exposure alone");
  engine::core::cvar_set_bool("r_auto_exposure", true);
  CHECK(adaptation_frame(20.2F), "turning it back on adapts");
  CHECK(g_adaptParams[3] == 0.0F,
        "and starts from the scene, not the stale exposure");

  engine::core::cvar_set_float("r_exposure", 1.0F);
  engine::core::cvar_set_float("r_auto_exposure_speed", 1.5F);
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Command Buffer Post Chain Unit Tests ===\n");

  if (!engine::core::initialize_cvars()) {
    std::fprintf(stderr, "FAIL: cvar init\n");
    return 1;
  }
  // The chain tests run with auto exposure on, so its chain is built.
  if (!engine::core::cvar_register_bool("r_bloom", true, "bloom toggle") ||
      !engine::core::cvar_register_bool("r_auto_exposure", true, "test") ||
      !engine::core::cvar_register_float("r_auto_exposure_speed", 1.5F,
                                         "test") ||
      !engine::core::cvar_register_float("r_exposure", 1.0F, "test")) {
    std::fprintf(stderr, "FAIL: cvar register\n");
    return 1;
  }

  test_framebuffer_failure_disables_post_chains();
  test_texture_failure_disables_post_chains();
  test_success_creates_chains_once_per_size();
  test_auto_exposure_adapts_on_the_device();

  engine::core::shutdown_cvars();
  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
