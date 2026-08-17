// Verifies pass resource lifetime behavior without requiring a real GL device.

#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"

#include <cstdio>
#include <cstdint>

namespace engine::renderer {

namespace {

struct FakeDeviceStats final {
  std::uint32_t nextId = 1U;
  std::uint32_t createCalls = 0U;
  std::uint32_t failCreateCall = 0U;
  int aliveTextures = 0;
  int aliveRenderTargets = 0;
  int mipChainTextures = 0;
  std::uint32_t boundRenderTarget = 0U;
  bool renderTargetComplete = true;
};

FakeDeviceStats g_stats{};
RenderDevice g_device{};

std::uint32_t make_resource(bool texture) noexcept {
  ++g_stats.createCalls;
  if ((g_stats.failCreateCall != 0U) &&
      (g_stats.createCalls == g_stats.failCreateCall)) {
    return 0U;
  }

  if (texture) {
    ++g_stats.aliveTextures;
  } else {
    ++g_stats.aliveRenderTargets;
  }
  return g_stats.nextId++;
}

DeviceTextureHandle fake_create_texture(const TextureDesc &desc) noexcept {
  if (desc.mipLevels != 1) {
    ++g_stats.mipChainTextures;
  }
  return DeviceTextureHandle{make_resource(true)};
}

void fake_destroy_texture(DeviceTextureHandle texture) noexcept {
  if (texture.value != 0U) {
    --g_stats.aliveTextures;
  }
}

RenderTargetHandle fake_create_render_target(
    const RenderTargetDesc &) noexcept {
  // Completeness is validated at creation under the device contract, so a
  // scripted incomplete target is a creation failure.
  if (!g_stats.renderTargetComplete) {
    ++g_stats.createCalls;
    return RenderTargetHandle{};
  }
  return RenderTargetHandle{make_resource(false)};
}

void fake_destroy_render_target(RenderTargetHandle target) noexcept {
  if (target.value != 0U) {
    --g_stats.aliveRenderTargets;
  }
}

void fake_bind_render_target(RenderTargetHandle target) noexcept {
  g_stats.boundRenderTarget = target.value;
}

void reset_device() noexcept {
  shutdown_pass_resources();

  g_stats = FakeDeviceStats{};
  g_device = RenderDevice{};
  g_device.create_texture = &fake_create_texture;
  g_device.destroy_texture = &fake_destroy_texture;
  g_device.create_render_target = &fake_create_render_target;
  g_device.destroy_render_target = &fake_destroy_render_target;
  g_device.bind_render_target = &fake_bind_render_target;
}

bool no_live_resources() noexcept {
  return (g_stats.aliveTextures == 0) && (g_stats.aliveRenderTargets == 0);
}

} // namespace

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

void test_success_shutdown_releases_all() noexcept {
  reset_device();

  CHECK(initialize_pass_resources(640, 480), "initialize succeeds");
  const PassResources &resources = get_pass_resources();
  CHECK(pass_resource_texture(resources.sceneColor) !=
            kInvalidDeviceTexture,
        "scene color texture is assigned");
  CHECK(g_stats.aliveTextures == 9, "all textures tracked alive");
  CHECK(g_stats.aliveRenderTargets == 5, "all render targets tracked alive");
  // Issue #229: only mip 0 is ever rendered, so no pass texture may ask
  // for a mip chain that would hold stale data forever.
  CHECK(g_stats.mipChainTextures == 0,
        "no pass texture requests a generated mip chain");

  shutdown_pass_resources();
  CHECK(no_live_resources(), "shutdown releases all resources");
}

void test_partial_failure_releases_created_resources() noexcept {
  reset_device();
  g_stats.failCreateCall = 7U;

  CHECK(!initialize_pass_resources(640, 480), "mid-creation failure rejected");
  CHECK(no_live_resources(), "partial failure releases created resources");
  CHECK(pass_resource_texture(PassResourceId{1U}) == kInvalidDeviceTexture,
        "failed initialize does not commit global state");
}

void test_incomplete_framebuffer_releases_created_resources() noexcept {
  reset_device();
  g_stats.renderTargetComplete = false;

  CHECK(!initialize_pass_resources(640, 480), "incomplete render target fails");
  CHECK(no_live_resources(), "completeness failure releases resources");
}

void test_resize_failure_keeps_existing_resources() noexcept {
  reset_device();

  CHECK(initialize_pass_resources(640, 480), "initial resources created");
  const PassResources resources = get_pass_resources();
  const DeviceTextureHandle oldSceneColor =
      pass_resource_texture(resources.sceneColor);
  const int oldTextureCount = g_stats.aliveTextures;
  const int oldRenderTargetCount = g_stats.aliveRenderTargets;

  g_stats.failCreateCall = g_stats.createCalls + 1U;
  resize_pass_resources(800, 600);

  CHECK(pass_resource_texture(resources.sceneColor) == oldSceneColor,
        "resize failure keeps old scene color");
  CHECK(g_stats.aliveTextures == oldTextureCount,
        "resize failure keeps old textures alive");
  CHECK(g_stats.aliveRenderTargets == oldRenderTargetCount,
        "resize failure keeps old render targets alive");

  shutdown_pass_resources();
  CHECK(no_live_resources(), "shutdown releases resources after resize failure");
}

/// EXPECTATION (audit H-12): resize reports its outcome — true for a
/// same-size no-op and a successful swap (which destroys exactly the old
/// target set), false for a failed recreation — so the flush can retry
/// instead of recording a size the targets never reached.
void test_resize_reports_status_and_swaps() noexcept {
  reset_device();

  CHECK(initialize_pass_resources(640, 480), "initial resources created");
  const PassResources resources = get_pass_resources();
  const DeviceTextureHandle oldSceneColor =
      pass_resource_texture(resources.sceneColor);
  const int oldTextureCount = g_stats.aliveTextures;
  const int oldRenderTargetCount = g_stats.aliveRenderTargets;

  CHECK(resize_pass_resources(640, 480), "same-size resize reports success");
  CHECK(pass_resource_texture(resources.sceneColor) == oldSceneColor,
        "same-size resize keeps targets");

  g_stats.failCreateCall = g_stats.createCalls + 3U;
  CHECK(!resize_pass_resources(1024, 768), "failed resize reports false");
  CHECK(pass_resource_texture(resources.sceneColor) == oldSceneColor,
        "failed resize keeps old targets");

  g_stats.failCreateCall = 0U;
  CHECK(resize_pass_resources(1024, 768), "retried resize reports success");
  CHECK(pass_resource_texture(resources.sceneColor) != oldSceneColor,
        "successful resize swaps to new targets");
  CHECK(g_stats.aliveTextures == oldTextureCount,
        "successful resize destroys exactly the old textures");
  CHECK(g_stats.aliveRenderTargets == oldRenderTargetCount,
        "successful resize destroys exactly the old render targets");

  shutdown_pass_resources();
  CHECK(no_live_resources(), "shutdown releases resources after retry");
}

} // namespace

int main() {
  std::printf("=== Pass Resources Unit Tests ===\n");

  test_success_shutdown_releases_all();
  test_partial_failure_releases_created_resources();
  test_incomplete_framebuffer_releases_created_resources();
  test_resize_failure_keeps_existing_resources();
  test_resize_reports_status_and_swaps();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
