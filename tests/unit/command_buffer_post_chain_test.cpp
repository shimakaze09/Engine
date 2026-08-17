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

#include <cstdint>
#include <cstdio>

namespace engine::renderer {

namespace {

/// Bookkeeping the fake device records so tests can assert on resource
/// lifetime, create-call volume, and draw destinations.
struct FakeDeviceStats final {
  std::uint32_t nextId = 1U;
  int hdrTextureCreates = 0;
  int renderTargetCreates = 0;
  int aliveTextures = 0;
  int aliveRenderTargets = 0;
  bool failHdrTextureCreate = false;
  bool failRenderTargetCreate = false;
  std::uint32_t boundRenderTarget = 0U;
  int draws = 0;
  int drawsToBackBuffer = 0;
};

FakeDeviceStats g_stats{};
RenderDevice g_device{};

DeviceTextureHandle fake_create_texture(const TextureDesc &desc) noexcept {
  // The RGBA16F post-chain allocations are the ones the audit N-10 tests
  // script failures for; other formats always succeed.
  if (desc.format == TextureFormat::RGBA16F) {
    ++g_stats.hdrTextureCreates;
    if (g_stats.failHdrTextureCreate) {
      return kInvalidDeviceTexture;
    }
  }
  ++g_stats.aliveTextures;
  return DeviceTextureHandle{g_stats.nextId++};
}

void fake_destroy_texture(DeviceTextureHandle texture) noexcept {
  if (texture.value != 0U) {
    --g_stats.aliveTextures;
  }
}

RenderTargetHandle fake_create_render_target(
    const RenderTargetDesc &desc) noexcept {
  ++g_stats.renderTargetCreates;
  if (g_stats.failRenderTargetCreate) {
    return RenderTargetHandle{};
  }
  // The contract rejects targets over failed (invalid) textures; mirroring
  // that here keeps "no target is created over a failed texture" honest.
  if ((desc.colorCount > 0U) &&
      (desc.colors[0].texture == kInvalidDeviceTexture)) {
    return RenderTargetHandle{};
  }
  ++g_stats.aliveRenderTargets;
  return RenderTargetHandle{g_stats.nextId++};
}

void fake_destroy_render_target(RenderTargetHandle target) noexcept {
  if (target.value != 0U) {
    --g_stats.aliveRenderTargets;
  }
}

void fake_bind_render_target(RenderTargetHandle target) noexcept {
  g_stats.boundRenderTarget = target.value;
}

void fake_draw(DeviceGeometryHandle, PrimitiveTopology, std::int32_t,
               std::int32_t) noexcept {
  ++g_stats.draws;
  if (g_stats.boundRenderTarget == 0U) {
    ++g_stats.drawsToBackBuffer;
  }
}

void fake_bind_program(DeviceProgramHandle) noexcept {}
void fake_bind_texture_slot(std::uint32_t, DeviceTextureHandle) noexcept {}
void fake_set_param_i32(ShaderParam, std::int32_t) noexcept {}
void fake_set_param_f32(ShaderParam, float) noexcept {}
void fake_set_param_vec2(ShaderParam, const float *) noexcept {}
void fake_set_viewport(std::int32_t, std::int32_t, std::int32_t,
                       std::int32_t) noexcept {}
void fake_apply_render_state(const RenderState &) noexcept {}
void fake_clear(ClearFlags, float, float, float, float) noexcept {}

/// Installs the fake device table and clears its stats.
void reset_fake_device() noexcept {
  g_stats = FakeDeviceStats{};
  g_device = RenderDevice{};
  g_device.create_texture = &fake_create_texture;
  g_device.destroy_texture = &fake_destroy_texture;
  g_device.create_render_target = &fake_create_render_target;
  g_device.destroy_render_target = &fake_destroy_render_target;
  g_device.bind_render_target = &fake_bind_render_target;
  g_device.draw = &fake_draw;
  g_device.bind_program = &fake_bind_program;
  g_device.bind_texture_slot = &fake_bind_texture_slot;
  g_device.set_param_i32 = &fake_set_param_i32;
  g_device.set_param_f32 = &fake_set_param_f32;
  g_device.set_param_vec2 = &fake_set_param_vec2;
  g_device.set_viewport = &fake_set_viewport;
  g_device.apply_render_state = &fake_apply_render_state;
  g_device.clear = &fake_clear;
}

} // namespace

const RenderDevice *render_device() noexcept { return &g_device; }

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
  return (g_stats.hdrTextureCreates - hdrBaseline) +
         (g_stats.renderTargetCreates - targetBaseline);
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

  g_stats.failRenderTargetCreate = true;
  const int aliveTextures = g_stats.aliveTextures;
  const int aliveRenderTargets = g_stats.aliveRenderTargets;
  FrameFlushContext ctx = make_context(lights, 640, 480);
  flush_post_chain(ctx);

  CHECK(g_stats.drawsToBackBuffer == 0,
        "no pass draws into the back buffer on chain failure");
  CHECK(bloom_chain_is_zeroed(backend), "failed bloom chain fully released");
  CHECK(luminance_chain_is_zeroed(backend),
        "failed luminance chain fully released");
  CHECK(g_stats.aliveTextures == aliveTextures,
        "chain failure leaks no textures");
  CHECK(g_stats.aliveRenderTargets == aliveRenderTargets,
        "chain failure leaks no render targets");
  CHECK(g_stats.draws > 0, "tonemap still runs into the final target");

  const int hdrBaseline = g_stats.hdrTextureCreates;
  const int targetBaseline = g_stats.renderTargetCreates;
  FrameFlushContext repeatCtx = make_context(lights, 640, 480);
  flush_post_chain(repeatCtx);
  CHECK(chain_create_calls_since(hdrBaseline, targetBaseline) == 0,
        "failed size is not retried every frame");
  CHECK(g_stats.drawsToBackBuffer == 0,
        "repeat frame still never draws into the back buffer");

  g_stats.failRenderTargetCreate = false;
  FrameFlushContext resizedCtx = make_context(lights, 800, 600);
  flush_post_chain(resizedCtx);
  CHECK(!bloom_chain_is_zeroed(backend), "resize retries the bloom chain");
  CHECK(!luminance_chain_is_zeroed(backend),
        "resize retries the luminance chain");
  CHECK(g_stats.drawsToBackBuffer == 0,
        "recovered chain draws only into offscreen targets");
}

/// EXPECTATION (audit N-10): a texture-create failure is handled the same
/// way and never hands an invalid texture to create_render_target (which
/// would build a target the completeness gate must reject).
void test_texture_failure_disables_post_chains() noexcept {
  CHECK(reset_post_chain_harness(), "pass resources initialize");
  static const SceneLightData lights{};
  BackendState &backend = backend_state();

  g_stats.failHdrTextureCreate = true;
  const int targetBaseline = g_stats.renderTargetCreates;
  FrameFlushContext ctx = make_context(lights, 640, 480);
  flush_post_chain(ctx);

  CHECK(g_stats.drawsToBackBuffer == 0,
        "no pass draws into the back buffer on texture failure");
  CHECK(bloom_chain_is_zeroed(backend), "failed bloom chain fully released");
  CHECK(luminance_chain_is_zeroed(backend),
        "failed luminance chain fully released");
  CHECK(g_stats.renderTargetCreates == targetBaseline,
        "no render target is created over a failed texture");

  const int hdrBaseline = g_stats.hdrTextureCreates;
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
  CHECK(g_stats.draws > 0, "post chain draws");
  CHECK(g_stats.drawsToBackBuffer == 0,
        "all post draws land in offscreen targets");

  const int hdrBaseline = g_stats.hdrTextureCreates;
  const int targetBaseline = g_stats.renderTargetCreates;
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
