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
  int aliveFramebuffers = 0;
  std::uint32_t boundFramebuffer = 0U;
  bool framebufferComplete = true;
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
    ++g_stats.aliveFramebuffers;
  }
  return g_stats.nextId++;
}

std::uint32_t fake_create_texture_2d(std::int32_t, std::int32_t, std::int32_t,
                                     const void *) noexcept {
  return make_resource(true);
}

std::uint32_t fake_create_texture_2d_hdr(std::int32_t, std::int32_t,
                                         std::int32_t,
                                         const float *) noexcept {
  return make_resource(true);
}

std::uint32_t fake_create_depth_texture(std::int32_t,
                                        std::int32_t) noexcept {
  return make_resource(true);
}

std::uint32_t fake_create_texture_2d_r32f(std::int32_t, std::int32_t,
                                          const float *) noexcept {
  return make_resource(true);
}

void fake_destroy_texture(std::uint32_t id) noexcept {
  if (id != 0U) {
    --g_stats.aliveTextures;
  }
}

std::uint32_t fake_create_framebuffer(std::uint32_t,
                                      std::uint32_t) noexcept {
  return make_resource(false);
}

std::uint32_t fake_create_framebuffer_mrt(const std::uint32_t *,
                                          std::int32_t,
                                          std::uint32_t) noexcept {
  return make_resource(false);
}

void fake_destroy_framebuffer(std::uint32_t id) noexcept {
  if (id != 0U) {
    --g_stats.aliveFramebuffers;
  }
}

void fake_bind_framebuffer(std::uint32_t fbo) noexcept {
  g_stats.boundFramebuffer = fbo;
}

bool fake_check_framebuffer_complete() noexcept {
  return g_stats.framebufferComplete;
}

void reset_device() noexcept {
  shutdown_pass_resources();

  g_stats = FakeDeviceStats{};
  g_device = RenderDevice{};
  g_device.create_texture_2d = &fake_create_texture_2d;
  g_device.create_texture_2d_hdr = &fake_create_texture_2d_hdr;
  g_device.create_depth_texture = &fake_create_depth_texture;
  g_device.destroy_texture = &fake_destroy_texture;
  g_device.create_framebuffer = &fake_create_framebuffer;
  g_device.create_framebuffer_mrt = &fake_create_framebuffer_mrt;
  g_device.destroy_framebuffer = &fake_destroy_framebuffer;
  g_device.bind_framebuffer = &fake_bind_framebuffer;
  g_device.check_framebuffer_complete = &fake_check_framebuffer_complete;
  g_device.create_texture_2d_r32f = &fake_create_texture_2d_r32f;
}

bool no_live_resources() noexcept {
  return (g_stats.aliveTextures == 0) && (g_stats.aliveFramebuffers == 0);
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
  CHECK(pass_resource_gpu_texture(resources.sceneColor) != 0U,
        "scene color texture is assigned");
  CHECK(g_stats.aliveTextures == 9, "all textures tracked alive");
  CHECK(g_stats.aliveFramebuffers == 5, "all framebuffers tracked alive");

  shutdown_pass_resources();
  CHECK(no_live_resources(), "shutdown releases all resources");
}

void test_partial_failure_releases_created_resources() noexcept {
  reset_device();
  g_stats.failCreateCall = 7U;

  CHECK(!initialize_pass_resources(640, 480), "mid-creation failure rejected");
  CHECK(no_live_resources(), "partial failure releases created resources");
  CHECK(pass_resource_gpu_texture(PassResourceId{1U}) == 0U,
        "failed initialize does not commit global state");
}

void test_incomplete_framebuffer_releases_created_resources() noexcept {
  reset_device();
  g_stats.framebufferComplete = false;

  CHECK(!initialize_pass_resources(640, 480), "incomplete framebuffer fails");
  CHECK(no_live_resources(), "completeness failure releases resources");
  CHECK(g_stats.boundFramebuffer == 0U, "failed completeness check unbinds");
}

void test_resize_failure_keeps_existing_resources() noexcept {
  reset_device();

  CHECK(initialize_pass_resources(640, 480), "initial resources created");
  const PassResources resources = get_pass_resources();
  const std::uint32_t oldSceneColor =
      pass_resource_gpu_texture(resources.sceneColor);
  const int oldTextureCount = g_stats.aliveTextures;
  const int oldFramebufferCount = g_stats.aliveFramebuffers;

  g_stats.failCreateCall = g_stats.createCalls + 1U;
  resize_pass_resources(800, 600);

  CHECK(pass_resource_gpu_texture(resources.sceneColor) == oldSceneColor,
        "resize failure keeps old scene color");
  CHECK(g_stats.aliveTextures == oldTextureCount,
        "resize failure keeps old textures alive");
  CHECK(g_stats.aliveFramebuffers == oldFramebufferCount,
        "resize failure keeps old framebuffers alive");

  shutdown_pass_resources();
  CHECK(no_live_resources(), "shutdown releases resources after resize failure");
}

} // namespace

int main() {
  std::printf("=== Pass Resources Unit Tests ===\n");

  test_success_shutdown_releases_all();
  test_partial_failure_releases_created_resources();
  test_incomplete_framebuffer_releases_created_resources();
  test_resize_failure_keeps_existing_resources();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
