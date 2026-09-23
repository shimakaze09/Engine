// Verifies the production post chain (flush_post_chain) against a fake
// render device: bloom/luminance mip-chain creation is transactional, a
// failed chain disables the pass instead of rendering into the default
// framebuffer, failures retry on resize rather than every frame, and a
// healthy chain is created exactly once per size (audit N-10).

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "command_buffer_post_resources.h"
#include "engine/core/cvar.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"

#include "../fake_render_device.h"

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
  device.bind_render_target = &tests::fake::bind_render_target;
  device.draw = &tests::fake::draw;
  device.bind_program = &tests::fake::bind_program;
  device.bind_texture_slot = &tests::fake::bind_texture_slot;
  device.set_param_i32 = &tests::fake::set_param_i32;
  device.set_param_f32 = &tests::fake::set_param_f32;
  device.set_param_vec2 = &tests::fake::set_param_vec2;
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

/// Returns true when every luminance mip texture and render-target slot is
/// invalid.
bool luminance_chain_is_zeroed(const BackendState &backend) noexcept {
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if ((backend.lumMipTextures[i] != kInvalidDeviceTexture) ||
        (backend.lumMipTargets[i].value != 0U)) {
      return false;
    }
  }
  return true;
}

/// Builds a fresh flush context over the shared backend and current pass
/// resources at the given drawable size.
FrameFlushContext make_context(const SceneLightData &lights, int width,
                               int height) noexcept {
  return FrameFlushContext{.backend = backend_state(),
                           .dev = render_device(),
                           .commandBufferView = {},
                           .registry = nullptr,
                           .lights = lights,
                           .timeSeconds = 0.0F,
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

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Command Buffer Post Chain Unit Tests ===\n");

  if (!engine::core::initialize_cvars()) {
    std::fprintf(stderr, "FAIL: cvar init\n");
    return 1;
  }
  if (!engine::core::cvar_register_bool("r_bloom", true, "bloom toggle")) {
    std::fprintf(stderr, "FAIL: cvar register\n");
    return 1;
  }

  test_framebuffer_failure_disables_post_chains();
  test_texture_failure_disables_post_chains();
  test_success_creates_chains_once_per_size();

  engine::core::shutdown_cvars();
  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
