#include "engine/renderer/pass_resources.h"

#include <cstdint>

#include "engine/core/logging.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

namespace {

// Resource slot 1 = scene color (RGBA16F).
// Resource slot 2 = scene depth (DEPTH24).
// Resource slot 3 = final color (back buffer — no GPU texture needed).
constexpr std::uint32_t kSceneColorSlot = 1U;
constexpr std::uint32_t kSceneDepthSlot = 2U;
constexpr std::uint32_t kFinalColorSlot = 3U;
constexpr std::uint32_t kGBufferAlbedoSlot = 4U;
constexpr std::uint32_t kGBufferNormalSlot = 5U;
constexpr std::uint32_t kGBufferEmissiveSlot = 6U;
constexpr std::uint32_t kGBufferDepthSlot = 7U;

struct PassResourceState final {
  bool initialized = false;
  int width = 0;
  int height = 0;

  std::uint32_t sceneColorTexture = 0U;
  std::uint32_t sceneDepthTexture = 0U;
  std::uint32_t sceneFbo = 0U;

  std::uint32_t finalColorTexture = 0U;
  std::uint32_t finalFbo = 0U;

  // G-Buffer textures (deferred path).
  std::uint32_t gbufferAlbedoTex = 0U;   // RGBA8 — albedo.rgb + metallic.a
  std::uint32_t gbufferNormalTex = 0U;   // RGBA16F — normal.xyz + roughness.a
  std::uint32_t gbufferEmissiveTex = 0U; // RGBA8 — emissive.rgb + AO.a
  std::uint32_t gbufferDepthTex = 0U;    // DEPTH24
  std::uint32_t gbufferFbo = 0U;

  PassResources resources{};
};

PassResourceState g_state{};

void destroy_gpu_resources() noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
  }

  // G-Buffer (reverse order of creation).
  if (g_state.gbufferFbo != 0U) {
    dev->destroy_framebuffer(g_state.gbufferFbo);
    g_state.gbufferFbo = 0U;
  }
  if (g_state.gbufferDepthTex != 0U) {
    dev->destroy_texture(g_state.gbufferDepthTex);
    g_state.gbufferDepthTex = 0U;
  }
  if (g_state.gbufferEmissiveTex != 0U) {
    dev->destroy_texture(g_state.gbufferEmissiveTex);
    g_state.gbufferEmissiveTex = 0U;
  }
  if (g_state.gbufferNormalTex != 0U) {
    dev->destroy_texture(g_state.gbufferNormalTex);
    g_state.gbufferNormalTex = 0U;
  }
  if (g_state.gbufferAlbedoTex != 0U) {
    dev->destroy_texture(g_state.gbufferAlbedoTex);
    g_state.gbufferAlbedoTex = 0U;
  }

  // Forward path.
  if (g_state.finalFbo != 0U) {
    dev->destroy_framebuffer(g_state.finalFbo);
    g_state.finalFbo = 0U;
  }
  if (g_state.finalColorTexture != 0U) {
    dev->destroy_texture(g_state.finalColorTexture);
    g_state.finalColorTexture = 0U;
  }
  if (g_state.sceneFbo != 0U) {
    dev->destroy_framebuffer(g_state.sceneFbo);
    g_state.sceneFbo = 0U;
  }
  if (g_state.sceneColorTexture != 0U) {
    dev->destroy_texture(g_state.sceneColorTexture);
    g_state.sceneColorTexture = 0U;
  }
  if (g_state.sceneDepthTexture != 0U) {
    dev->destroy_texture(g_state.sceneDepthTexture);
    g_state.sceneDepthTexture = 0U;
  }
}

bool create_gpu_resources(int width, int height) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return false;
  }

  // Scene color: RGBA16F (via create_texture_2d_hdr with nullptr data).
  g_state.sceneColorTexture =
      dev->create_texture_2d_hdr(static_cast<std::int32_t>(width),
                                 static_cast<std::int32_t>(height), 4, nullptr);
  if (g_state.sceneColorTexture == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create scene color texture");
    return false;
  }

  // Scene depth: DEPTH24.
  g_state.sceneDepthTexture = dev->create_depth_texture(
      static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
  if (g_state.sceneDepthTexture == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create scene depth texture");
    dev->destroy_texture(g_state.sceneColorTexture);
    g_state.sceneColorTexture = 0U;
    return false;
  }

  // Scene FBO.
  g_state.sceneFbo = dev->create_framebuffer(g_state.sceneColorTexture,
                                             g_state.sceneDepthTexture);
  if (g_state.sceneFbo == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create scene framebuffer");
    dev->destroy_texture(g_state.sceneDepthTexture);
    g_state.sceneDepthTexture = 0U;
    dev->destroy_texture(g_state.sceneColorTexture);
    g_state.sceneColorTexture = 0U;
    return false;
  }

  // Final color: RGBA8 LDR (tonemapped output for editor viewport).
  g_state.finalColorTexture =
      dev->create_texture_2d(static_cast<std::int32_t>(width),
                             static_cast<std::int32_t>(height), 4, nullptr);
  if (g_state.finalColorTexture == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create final color texture");
    dev->destroy_framebuffer(g_state.sceneFbo);
    g_state.sceneFbo = 0U;
    dev->destroy_texture(g_state.sceneDepthTexture);
    g_state.sceneDepthTexture = 0U;
    dev->destroy_texture(g_state.sceneColorTexture);
    g_state.sceneColorTexture = 0U;
    return false;
  }

  // Final FBO (color-only, no depth).
  g_state.finalFbo = dev->create_framebuffer(g_state.finalColorTexture, 0U);
  if (g_state.finalFbo == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create final framebuffer");
    dev->destroy_texture(g_state.finalColorTexture);
    g_state.finalColorTexture = 0U;
    dev->destroy_framebuffer(g_state.sceneFbo);
    g_state.sceneFbo = 0U;
    dev->destroy_texture(g_state.sceneDepthTexture);
    g_state.sceneDepthTexture = 0U;
    dev->destroy_texture(g_state.sceneColorTexture);
    g_state.sceneColorTexture = 0U;
    return false;
  }

  g_state.width = width;
  g_state.height = height;

  g_state.resources.sceneColor = PassResourceId{kSceneColorSlot};
  g_state.resources.sceneDepth = PassResourceId{kSceneDepthSlot};
  g_state.resources.finalColor = PassResourceId{kFinalColorSlot};

  // --- G-Buffer textures (deferred path) ---
  const auto w32 = static_cast<std::int32_t>(width);
  const auto h32 = static_cast<std::int32_t>(height);

  // RT0: albedo (RGBA8).
  g_state.gbufferAlbedoTex = dev->create_texture_2d(w32, h32, 4, nullptr);
  if (g_state.gbufferAlbedoTex == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create G-Buffer albedo texture");
    return false;
  }

  // RT1: normals + roughness (RGBA16F).
  g_state.gbufferNormalTex = dev->create_texture_2d_hdr(w32, h32, 4, nullptr);
  if (g_state.gbufferNormalTex == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create G-Buffer normal texture");
    return false;
  }

  // RT2: emissive + AO (RGBA8).
  g_state.gbufferEmissiveTex = dev->create_texture_2d(w32, h32, 4, nullptr);
  if (g_state.gbufferEmissiveTex == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create G-Buffer emissive texture");
    return false;
  }

  // G-Buffer depth (DEPTH24).
  g_state.gbufferDepthTex = dev->create_depth_texture(w32, h32);
  if (g_state.gbufferDepthTex == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create G-Buffer depth texture");
    return false;
  }

  // G-Buffer MRT FBO (3 color + 1 depth).
  const std::uint32_t gbufferColors[] = {g_state.gbufferAlbedoTex,
                                         g_state.gbufferNormalTex,
                                         g_state.gbufferEmissiveTex};
  g_state.gbufferFbo =
      dev->create_framebuffer_mrt(gbufferColors, 3, g_state.gbufferDepthTex);
  if (g_state.gbufferFbo == 0U) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to create G-Buffer framebuffer");
    return false;
  }

  // Verify G-Buffer FBO completeness.
  dev->bind_framebuffer(g_state.gbufferFbo);
  if (!dev->check_framebuffer_complete()) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "G-Buffer FBO is not complete");
    dev->bind_framebuffer(0U);
    return false;
  }
  dev->bind_framebuffer(0U);

  g_state.resources.gbufferAlbedo = PassResourceId{kGBufferAlbedoSlot};
  g_state.resources.gbufferNormal = PassResourceId{kGBufferNormalSlot};
  g_state.resources.gbufferEmissive = PassResourceId{kGBufferEmissiveSlot};
  g_state.resources.gbufferDepth = PassResourceId{kGBufferDepthSlot};

  return true;
}

} // namespace

bool initialize_pass_resources(int width, int height) noexcept {
  if (g_state.initialized) {
    return true;
  }

  if ((width <= 0) || (height <= 0)) {
    return false;
  }

  if (!create_gpu_resources(width, height)) {
    return false;
  }

  g_state.initialized = true;
  return true;
}

void shutdown_pass_resources() noexcept {
  if (!g_state.initialized) {
    return;
  }

  destroy_gpu_resources();
  g_state = PassResourceState{};
}

void resize_pass_resources(int width, int height) noexcept {
  if (!g_state.initialized) {
    return;
  }

  if ((width <= 0) || (height <= 0)) {
    return;
  }

  if ((width == g_state.width) && (height == g_state.height)) {
    return;
  }

  destroy_gpu_resources();
  if (!create_gpu_resources(width, height)) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to recreate pass resources on resize");
    g_state.initialized = false;
  }
}

const PassResources &get_pass_resources() noexcept { return g_state.resources; }

std::uint32_t pass_resource_gpu_texture(PassResourceId resource) noexcept {
  if (resource.id == kSceneColorSlot) {
    return g_state.sceneColorTexture;
  }
  if (resource.id == kSceneDepthSlot) {
    return g_state.sceneDepthTexture;
  }
  if (resource.id == kFinalColorSlot) {
    return g_state.finalColorTexture;
  }
  if (resource.id == kGBufferAlbedoSlot) {
    return g_state.gbufferAlbedoTex;
  }
  if (resource.id == kGBufferNormalSlot) {
    return g_state.gbufferNormalTex;
  }
  if (resource.id == kGBufferEmissiveSlot) {
    return g_state.gbufferEmissiveTex;
  }
  if (resource.id == kGBufferDepthSlot) {
    return g_state.gbufferDepthTex;
  }
  return 0U;
}

std::uint32_t
pass_resource_framebuffer(PassResourceId colorAttachment) noexcept {
  if (colorAttachment.id == kSceneColorSlot) {
    return g_state.sceneFbo;
  }
  if (colorAttachment.id == kFinalColorSlot) {
    return g_state.finalFbo;
  }
  if (colorAttachment.id == kGBufferAlbedoSlot) {
    return g_state.gbufferFbo;
  }
  return 0U;
}

} // namespace engine::renderer
