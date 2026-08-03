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
  int framebufferCreates = 0;
  int aliveTextures = 0;
  int aliveFramebuffers = 0;
  bool failHdrTextureCreate = false;
  bool failFramebufferCreate = false;
  std::uint32_t boundFramebuffer = 0U;
  int draws = 0;
  int drawsToDefaultFramebuffer = 0;
};

FakeDeviceStats g_stats{};
RenderDevice g_device{};

std::uint32_t fake_create_texture_2d(std::int32_t, std::int32_t, std::int32_t,
                                     const void *) noexcept {
  ++g_stats.aliveTextures;
  return g_stats.nextId++;
}

std::uint32_t fake_create_texture_2d_hdr(std::int32_t, std::int32_t,
                                         std::int32_t,
                                         const float *) noexcept {
  ++g_stats.hdrTextureCreates;
  if (g_stats.failHdrTextureCreate) {
    return 0U;
  }
  ++g_stats.aliveTextures;
  return g_stats.nextId++;
}

std::uint32_t fake_create_depth_texture(std::int32_t, std::int32_t) noexcept {
  ++g_stats.aliveTextures;
  return g_stats.nextId++;
}

std::uint32_t fake_create_texture_2d_r32f(std::int32_t, std::int32_t,
                                          const float *) noexcept {
  ++g_stats.aliveTextures;
  return g_stats.nextId++;
}

void fake_destroy_texture(std::uint32_t id) noexcept {
  if (id != 0U) {
    --g_stats.aliveTextures;
  }
}

std::uint32_t fake_create_framebuffer(std::uint32_t, std::uint32_t) noexcept {
  ++g_stats.framebufferCreates;
  if (g_stats.failFramebufferCreate) {
    return 0U;
  }
  ++g_stats.aliveFramebuffers;
  return g_stats.nextId++;
}

std::uint32_t fake_create_framebuffer_mrt(const std::uint32_t *, std::int32_t,
                                          std::uint32_t) noexcept {
  ++g_stats.aliveFramebuffers;
  return g_stats.nextId++;
}

void fake_destroy_framebuffer(std::uint32_t id) noexcept {
  if (id != 0U) {
    --g_stats.aliveFramebuffers;
  }
}

void fake_bind_framebuffer(std::uint32_t fbo) noexcept {
  g_stats.boundFramebuffer = fbo;
}

bool fake_check_framebuffer_complete() noexcept { return true; }

void fake_draw_arrays_triangles(std::int32_t, std::int32_t) noexcept {
  ++g_stats.draws;
  if (g_stats.boundFramebuffer == 0U) {
    ++g_stats.drawsToDefaultFramebuffer;
  }
}

void fake_bind_program(std::uint32_t) noexcept {}
void fake_bind_texture(std::int32_t, std::uint32_t) noexcept {}
void fake_bind_vertex_array(std::uint32_t) noexcept {}
void fake_set_uniform_int(std::int32_t, std::int32_t) noexcept {}
void fake_set_uniform_float(std::int32_t, float) noexcept {}
void fake_set_uniform_vec2(std::int32_t, const float *) noexcept {}
void fake_set_viewport(std::int32_t, std::int32_t, std::int32_t,
                       std::int32_t) noexcept {}
void fake_enable_depth_test() noexcept {}
void fake_disable_depth_test() noexcept {}
void fake_set_clear_color(float, float, float, float) noexcept {}
void fake_clear_color_depth() noexcept {}

/// Installs the fake device table and clears its stats.
void reset_fake_device() noexcept {
  g_stats = FakeDeviceStats{};
  g_device = RenderDevice{};
  g_device.create_texture_2d = &fake_create_texture_2d;
  g_device.create_texture_2d_hdr = &fake_create_texture_2d_hdr;
  g_device.create_depth_texture = &fake_create_depth_texture;
  g_device.create_texture_2d_r32f = &fake_create_texture_2d_r32f;
  g_device.destroy_texture = &fake_destroy_texture;
  g_device.create_framebuffer = &fake_create_framebuffer;
  g_device.create_framebuffer_mrt = &fake_create_framebuffer_mrt;
  g_device.destroy_framebuffer = &fake_destroy_framebuffer;
  g_device.bind_framebuffer = &fake_bind_framebuffer;
  g_device.check_framebuffer_complete = &fake_check_framebuffer_complete;
  g_device.draw_arrays_triangles = &fake_draw_arrays_triangles;
  g_device.bind_program = &fake_bind_program;
  g_device.bind_texture = &fake_bind_texture;
  g_device.bind_vertex_array = &fake_bind_vertex_array;
  g_device.set_uniform_int = &fake_set_uniform_int;
  g_device.set_uniform_float = &fake_set_uniform_float;
  g_device.set_uniform_vec2 = &fake_set_uniform_vec2;
  g_device.set_viewport = &fake_set_viewport;
  g_device.enable_depth_test = &fake_enable_depth_test;
  g_device.disable_depth_test = &fake_disable_depth_test;
  g_device.set_clear_color = &fake_set_clear_color;
  g_device.clear_color_depth = &fake_clear_color_depth;
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

/// Number of bloom+luminance HDR texture / framebuffer create calls issued
/// since the given baselines.
int chain_create_calls_since(int hdrBaseline, int fboBaseline) noexcept {
  return (g_stats.hdrTextureCreates - hdrBaseline) +
         (g_stats.framebufferCreates - fboBaseline);
}

/// Returns true when every bloom mip texture and framebuffer slot is zero.
bool bloom_chain_is_zeroed(const BackendState &backend) noexcept {
  for (int i = 0; i < BackendState::kBloomMipLevels; ++i) {
    if ((backend.bloomMipTextures[i] != 0U) ||
        (backend.bloomMipFbos[i] != 0U)) {
      return false;
    }
  }
  return true;
}

/// Returns true when every luminance mip texture and framebuffer slot is zero.
bool luminance_chain_is_zeroed(const BackendState &backend) noexcept {
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if ((backend.lumMipTextures[i] != 0U) || (backend.lumMipFbos[i] != 0U)) {
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
                           .envSkyboxTexture = 0U,
                           .iblPrefilteredTex = 0U,
                           .iblIrradianceTex = 0U,
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
  backend.bloomThresholdProgram = 1U;
  backend.bloomDownsampleProgram = 2U;
  backend.bloomUpsampleProgram = 3U;
  backend.tonemapProgram = 4U;
  backend.luminanceProgram = 5U;
  backend.autoExposureAvailable = true;
  backend.emptyVao = 6U;
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

  g_stats.failFramebufferCreate = true;
  const int aliveTextures = g_stats.aliveTextures;
  const int aliveFramebuffers = g_stats.aliveFramebuffers;
  FrameFlushContext ctx = make_context(lights, 640, 480);
  flush_post_chain(ctx);

  CHECK(g_stats.drawsToDefaultFramebuffer == 0,
        "no pass draws into the default framebuffer on chain failure");
  CHECK(bloom_chain_is_zeroed(backend), "failed bloom chain fully released");
  CHECK(luminance_chain_is_zeroed(backend),
        "failed luminance chain fully released");
  CHECK(g_stats.aliveTextures == aliveTextures,
        "chain failure leaks no textures");
  CHECK(g_stats.aliveFramebuffers == aliveFramebuffers,
        "chain failure leaks no framebuffers");
  CHECK(g_stats.draws > 0, "tonemap still runs into the final target");

  const int hdrBaseline = g_stats.hdrTextureCreates;
  const int fboBaseline = g_stats.framebufferCreates;
  FrameFlushContext repeatCtx = make_context(lights, 640, 480);
  flush_post_chain(repeatCtx);
  CHECK(chain_create_calls_since(hdrBaseline, fboBaseline) == 0,
        "failed size is not retried every frame");
  CHECK(g_stats.drawsToDefaultFramebuffer == 0,
        "repeat frame still never draws into the default framebuffer");

  g_stats.failFramebufferCreate = false;
  FrameFlushContext resizedCtx = make_context(lights, 800, 600);
  flush_post_chain(resizedCtx);
  CHECK(!bloom_chain_is_zeroed(backend), "resize retries the bloom chain");
  CHECK(!luminance_chain_is_zeroed(backend),
        "resize retries the luminance chain");
  CHECK(g_stats.drawsToDefaultFramebuffer == 0,
        "recovered chain draws only into offscreen targets");
}

/// EXPECTATION (audit N-10): a texture-create failure is handled the same
/// way and never hands a zero texture to create_framebuffer (which would
/// build an attachment-less framebuffer that skips the completeness gate).
void test_texture_failure_disables_post_chains() noexcept {
  CHECK(reset_post_chain_harness(), "pass resources initialize");
  static const SceneLightData lights{};
  BackendState &backend = backend_state();

  g_stats.failHdrTextureCreate = true;
  const int fboBaseline = g_stats.framebufferCreates;
  FrameFlushContext ctx = make_context(lights, 640, 480);
  flush_post_chain(ctx);

  CHECK(g_stats.drawsToDefaultFramebuffer == 0,
        "no pass draws into the default framebuffer on texture failure");
  CHECK(bloom_chain_is_zeroed(backend), "failed bloom chain fully released");
  CHECK(luminance_chain_is_zeroed(backend),
        "failed luminance chain fully released");
  CHECK(g_stats.framebufferCreates == fboBaseline,
        "no framebuffer is created over a failed texture");

  const int hdrBaseline = g_stats.hdrTextureCreates;
  FrameFlushContext repeatCtx = make_context(lights, 640, 480);
  flush_post_chain(repeatCtx);
  CHECK(chain_create_calls_since(hdrBaseline, fboBaseline) == 0,
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
  CHECK(g_stats.drawsToDefaultFramebuffer == 0,
        "all post draws land in offscreen targets");

  const int hdrBaseline = g_stats.hdrTextureCreates;
  const int fboBaseline = g_stats.framebufferCreates;
  FrameFlushContext repeatCtx = make_context(lights, 640, 480);
  flush_post_chain(repeatCtx);
  CHECK(chain_create_calls_since(hdrBaseline, fboBaseline) == 0,
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
