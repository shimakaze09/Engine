// Verifies the wrap mode each post-chain and pass texture is created with
// (issue #565 row 1), through a recording device over the production
// resource code. The rule has two halves and they pull opposite ways:
//
//   * A target that a later pass samples by screen position clamps at its
//     edges. Bloom, FXAA and SSAO all tap a little past a pixel's own
//     position, and with a repeating wrap a tap past one screen edge
//     returns the opposite edge. On base every one of these repeated.
//   * The SSAO rotation noise repeats. The pass tiles its 4x4 vectors
//     across the screen by sampling at uv * (drawable / 4); clamped, every
//     pixel past the first four reads the same edge texel. The change that
//     clamped the targets clamped this too, and SSAO's contribution to a
//     test frame fell from 0.84 levels a pixel to 0.015.
//
// The image-level consequences are covered on the GPU
// (engine_integration_post_edge_wrap_gpu, ..._brdf_lut_edge_gpu); this
// suite is what holds each creation site on every platform.

#include "command_buffer_context.h"
#include "command_buffer_post_resources.h"

#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace engine::renderer {

namespace {

/// Every texture the code under test asked the device for.
struct CreatedTexture final {
  TextureDesc desc{};
};

constexpr std::size_t kMaxCreated = 128U;
CreatedTexture g_created[kMaxCreated]{};
std::size_t g_createdCount = 0U;
std::uint32_t g_nextHandle = 1U;
RenderDevice g_device{};

DeviceTextureHandle record_create_texture(const TextureDesc &desc) noexcept {
  if (g_createdCount < kMaxCreated) {
    g_created[g_createdCount].desc = desc;
    // The initial pixels belong to the caller's stack frame.
    g_created[g_createdCount].desc.pixels = nullptr;
    ++g_createdCount;
  }
  return DeviceTextureHandle{g_nextHandle++};
}

void ignore_destroy_texture(DeviceTextureHandle) noexcept {}

RenderTargetHandle record_create_target(const RenderTargetDesc &) noexcept {
  return RenderTargetHandle{g_nextHandle++};
}

void ignore_destroy_target(RenderTargetHandle) noexcept {}

void reset_recording() noexcept {
  g_createdCount = 0U;
  g_device = RenderDevice{};
  g_device.create_texture = &record_create_texture;
  g_device.destroy_texture = &ignore_destroy_texture;
  g_device.create_render_target = &record_create_target;
  g_device.destroy_render_target = &ignore_destroy_target;
}

} // namespace

// Link seam for the resource TUs: the device they create through.
const RenderDevice *render_device() noexcept { return &g_device; }

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

/// How many of the recorded textures do not have the given wrap.
std::size_t count_without_wrap(TextureWrap wrap) noexcept {
  std::size_t count = 0U;
  for (std::size_t i = 0U; i < g_createdCount; ++i) {
    if (g_created[i].desc.wrap != wrap) {
      ++count;
    }
  }
  return count;
}

/// EXPECTATION: every level of the bloom chain clamps. Each level is
/// sampled with taps spread around the output pixel, and the coarsest
/// levels are a few texels across, so a tap past the edge is the rule
/// there rather than the exception.
void test_bloom_chain_clamps() noexcept {
  reset_recording();
  BackendState backend{};
  CHECK(ensure_bloom_resources(backend, 1280, 720), "bloom chain allocates");
  CHECK(g_createdCount >= 2U, "the chain has more than one level");
  CHECK(count_without_wrap(TextureWrap::ClampEdge) == 0U,
        "every bloom level clamps at its edges");
}

/// EXPECTATION: the luminance averaging chain clamps, for the same reason.
void test_luminance_chain_clamps() noexcept {
  reset_recording();
  BackendState backend{};
  CHECK(ensure_luminance_resources(backend, 1280, 720),
        "luminance chain allocates");
  CHECK(g_createdCount >= 1U, "the chain has a level");
  CHECK(count_without_wrap(TextureWrap::ClampEdge) == 0U,
        "every luminance level clamps at its edges");
}

/// EXPECTATION: every pass resource clamps — colour, G-buffer, the R32F
/// and depth targets alike. FXAA reads scene colour beside each pixel and
/// SSAO reads depth and normals at projected offsets that leave the screen
/// near its border.
void test_pass_resources_clamp() noexcept {
  reset_recording();
  CHECK(initialize_pass_resources(1280, 720), "pass resources allocate");
  CHECK(g_createdCount >= 4U, "the pass list creates its targets");
  CHECK(count_without_wrap(TextureWrap::ClampEdge) == 0U,
        "every pass resource clamps at its edges");
  shutdown_pass_resources();
}

/// EXPECTATION: the SSAO rotation noise is the exception: 4x4 and
/// repeating, because the pass tiles it.
void test_ssao_noise_repeats() noexcept {
  reset_recording();
  const DeviceTextureHandle noise = create_ssao_noise_texture();
  CHECK(noise != kInvalidDeviceTexture, "the noise texture is created");
  CHECK(g_createdCount == 1U, "one texture");
  if (g_createdCount == 1U) {
    CHECK((g_created[0].desc.width == 4) && (g_created[0].desc.height == 4),
          "the noise is 4x4, the period the pass's noise scale assumes");
    CHECK(g_created[0].desc.wrap == TextureWrap::Repeat,
          "the noise repeats so it tiles across the screen");
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  test_bloom_chain_clamps();
  test_luminance_chain_clamps();
  test_pass_resources_clamp();
  test_ssao_noise_repeats();
  if (g_failures != 0) {
    std::fprintf(stderr, "render_target_wrap_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("render_target_wrap_test: all tests passed\n");
  return 0;
}
