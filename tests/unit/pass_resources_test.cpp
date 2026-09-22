// Verifies pass resource lifetime behavior without requiring a real GL device.

#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"

#include "../fake_render_device.h"

#include <cstdio>
#include <cstdint>

namespace engine::renderer {

namespace {

// Textures created with a mip chain; pass resources never ask for one.
int g_mipChainTextures = 0;

DeviceTextureHandle count_mip_chains(const TextureDesc &desc) noexcept {
  if (desc.mipLevels != 1) {
    ++g_mipChainTextures;
  }
  return tests::fake::create_texture(desc);
}

void reset_device() noexcept {
  shutdown_pass_resources();

  tests::reset_fake_device();
  g_mipChainTextures = 0;
  RenderDevice &device = tests::fake_device();
  device.create_texture = &count_mip_chains;
  device.destroy_texture = &tests::fake::destroy_texture;
  device.create_render_target = &tests::fake::create_render_target;
  device.destroy_render_target = &tests::fake::destroy_render_target;
  device.bind_render_target = &tests::fake::bind_render_target;
}

int alive_textures() noexcept {
  return tests::fake_alive(tests::FakeKind::Texture);
}

int alive_render_targets() noexcept {
  return tests::fake_alive(tests::FakeKind::RenderTarget);
}

bool no_live_resources() noexcept {
  return (alive_textures() == 0) && (alive_render_targets() == 0);
}

} // namespace

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
  CHECK(alive_textures() == 9, "all textures tracked alive");
  CHECK(alive_render_targets() == 5, "all render targets tracked alive");
  // Issue #229: only mip 0 is ever rendered, so no pass texture may ask
  // for a mip chain that would hold stale data forever.
  CHECK(g_mipChainTextures == 0,
        "no pass texture requests a generated mip chain");

  shutdown_pass_resources();
  CHECK(no_live_resources(), "shutdown releases all resources");
}

void test_partial_failure_releases_created_resources() noexcept {
  reset_device();
  engine::tests::fake_log().failCreateCall = 7U;

  CHECK(!initialize_pass_resources(640, 480), "mid-creation failure rejected");
  CHECK(no_live_resources(), "partial failure releases created resources");
  CHECK(pass_resource_texture(PassResourceId{1U}) == kInvalidDeviceTexture,
        "failed initialize does not commit global state");
}

void test_incomplete_framebuffer_releases_created_resources() noexcept {
  reset_device();
  // Completeness is validated at creation under the device contract, so a
  // scripted incomplete target is a creation failure.
  engine::tests::fake_log().failKinds =
      engine::tests::fake_kind_bit(engine::tests::FakeKind::RenderTarget);

  CHECK(!initialize_pass_resources(640, 480), "incomplete render target fails");
  CHECK(no_live_resources(), "completeness failure releases resources");
}

void test_resize_failure_keeps_existing_resources() noexcept {
  reset_device();

  CHECK(initialize_pass_resources(640, 480), "initial resources created");
  const PassResources resources = get_pass_resources();
  const DeviceTextureHandle oldSceneColor =
      pass_resource_texture(resources.sceneColor);
  const int oldTextureCount = alive_textures();
  const int oldRenderTargetCount = alive_render_targets();

  engine::tests::fake_log().failCreateCall =
      engine::tests::fake_log().createCalls + 1U;
  resize_pass_resources(800, 600);

  CHECK(pass_resource_texture(resources.sceneColor) == oldSceneColor,
        "resize failure keeps old scene color");
  CHECK(alive_textures() == oldTextureCount,
        "resize failure keeps old textures alive");
  CHECK(alive_render_targets() == oldRenderTargetCount,
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
  const int oldTextureCount = alive_textures();
  const int oldRenderTargetCount = alive_render_targets();

  CHECK(resize_pass_resources(640, 480), "same-size resize reports success");
  CHECK(pass_resource_texture(resources.sceneColor) == oldSceneColor,
        "same-size resize keeps targets");

  engine::tests::fake_log().failCreateCall =
      engine::tests::fake_log().createCalls + 3U;
  CHECK(!resize_pass_resources(1024, 768), "failed resize reports false");
  CHECK(pass_resource_texture(resources.sceneColor) == oldSceneColor,
        "failed resize keeps old targets");

  engine::tests::fake_log().failCreateCall = 0U;
  CHECK(resize_pass_resources(1024, 768), "retried resize reports success");
  CHECK(pass_resource_texture(resources.sceneColor) != oldSceneColor,
        "successful resize swaps to new targets");
  CHECK(alive_textures() == oldTextureCount,
        "successful resize destroys exactly the old textures");
  CHECK(alive_render_targets() == oldRenderTargetCount,
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
