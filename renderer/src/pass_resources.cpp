// Implements pass resources behavior for the Engine renderer system.

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
constexpr std::uint32_t kSsaoTextureSlot = 8U;
constexpr std::uint32_t kSsaoBlurTextureSlot = 9U;

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

  // SSAO textures.
  std::uint32_t ssaoTex = 0U;           // R32F — raw AO
  std::uint32_t ssaoFbo = 0U;
  std::uint32_t ssaoBlurTex = 0U;       // R32F — blurred AO
  std::uint32_t ssaoBlurFbo = 0U;

  PassResources resources{};
};

PassResourceState g_state{};

/// Destroys or releases the requested object, handle, or resource for gpu resources.
void destroy_gpu_resources(PassResourceState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
  }

  // SSAO (reverse order of creation).
  if (state.ssaoBlurFbo != 0U) {
    dev->destroy_framebuffer(state.ssaoBlurFbo);
    state.ssaoBlurFbo = 0U;
  }
  if (state.ssaoBlurTex != 0U) {
    dev->destroy_texture(state.ssaoBlurTex);
    state.ssaoBlurTex = 0U;
  }
  if (state.ssaoFbo != 0U) {
    dev->destroy_framebuffer(state.ssaoFbo);
    state.ssaoFbo = 0U;
  }
  if (state.ssaoTex != 0U) {
    dev->destroy_texture(state.ssaoTex);
    state.ssaoTex = 0U;
  }

  // G-Buffer (reverse order of creation).
  if (state.gbufferFbo != 0U) {
    dev->destroy_framebuffer(state.gbufferFbo);
    state.gbufferFbo = 0U;
  }
  if (state.gbufferDepthTex != 0U) {
    dev->destroy_texture(state.gbufferDepthTex);
    state.gbufferDepthTex = 0U;
  }
  if (state.gbufferEmissiveTex != 0U) {
    dev->destroy_texture(state.gbufferEmissiveTex);
    state.gbufferEmissiveTex = 0U;
  }
  if (state.gbufferNormalTex != 0U) {
    dev->destroy_texture(state.gbufferNormalTex);
    state.gbufferNormalTex = 0U;
  }
  if (state.gbufferAlbedoTex != 0U) {
    dev->destroy_texture(state.gbufferAlbedoTex);
    state.gbufferAlbedoTex = 0U;
  }

  // Forward path.
  if (state.finalFbo != 0U) {
    dev->destroy_framebuffer(state.finalFbo);
    state.finalFbo = 0U;
  }
  if (state.finalColorTexture != 0U) {
    dev->destroy_texture(state.finalColorTexture);
    state.finalColorTexture = 0U;
  }
  if (state.sceneFbo != 0U) {
    dev->destroy_framebuffer(state.sceneFbo);
    state.sceneFbo = 0U;
  }
  if (state.sceneColorTexture != 0U) {
    dev->destroy_texture(state.sceneColorTexture);
    state.sceneColorTexture = 0U;
  }
  if (state.sceneDepthTexture != 0U) {
    dev->destroy_texture(state.sceneDepthTexture);
    state.sceneDepthTexture = 0U;
  }
}

bool fail_create(PassResourceState &state, const char *message) noexcept {
  core::log_message(core::LogLevel::Error, "pass_resources", message);
  destroy_gpu_resources(state);
  state = PassResourceState{};
  return false;
}

/// Creates a new object, handle, or resource for gpu resources.
bool create_gpu_resources(PassResourceState *outState, int width,
                          int height) noexcept {
  if (outState == nullptr) {
    return false;
  }

  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return false;
  }

  PassResourceState next{};
  next.width = width;
  next.height = height;

  // Scene color: RGBA16F (via create_texture_2d_hdr with nullptr data).
  next.sceneColorTexture =
      dev->create_texture_2d_hdr(static_cast<std::int32_t>(width),
                                 static_cast<std::int32_t>(height), 4, nullptr);
  if (next.sceneColorTexture == 0U) {
    return fail_create(next, "failed to create scene color texture");
  }

  // Scene depth: DEPTH24.
  next.sceneDepthTexture = dev->create_depth_texture(
      static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
  if (next.sceneDepthTexture == 0U) {
    return fail_create(next, "failed to create scene depth texture");
  }

  // Scene FBO.
  next.sceneFbo =
      dev->create_framebuffer(next.sceneColorTexture, next.sceneDepthTexture);
  if (next.sceneFbo == 0U) {
    return fail_create(next, "failed to create scene framebuffer");
  }

  // Final color: RGBA8 LDR (tonemapped output for editor viewport).
  next.finalColorTexture =
      dev->create_texture_2d(static_cast<std::int32_t>(width),
                             static_cast<std::int32_t>(height), 4, nullptr);
  if (next.finalColorTexture == 0U) {
    return fail_create(next, "failed to create final color texture");
  }

  // Final FBO (color-only, no depth).
  next.finalFbo = dev->create_framebuffer(next.finalColorTexture, 0U);
  if (next.finalFbo == 0U) {
    return fail_create(next, "failed to create final framebuffer");
  }

  next.resources.sceneColor = PassResourceId{kSceneColorSlot};
  next.resources.sceneDepth = PassResourceId{kSceneDepthSlot};
  next.resources.finalColor = PassResourceId{kFinalColorSlot};

  // --- G-Buffer textures (deferred path) ---
  const auto w32 = static_cast<std::int32_t>(width);
  const auto h32 = static_cast<std::int32_t>(height);

  // RT0: albedo (RGBA8).
  next.gbufferAlbedoTex = dev->create_texture_2d(w32, h32, 4, nullptr);
  if (next.gbufferAlbedoTex == 0U) {
    return fail_create(next, "failed to create G-Buffer albedo texture");
  }

  // RT1: normals + roughness (RGBA16F).
  next.gbufferNormalTex = dev->create_texture_2d_hdr(w32, h32, 4, nullptr);
  if (next.gbufferNormalTex == 0U) {
    return fail_create(next, "failed to create G-Buffer normal texture");
  }

  // RT2: emissive + AO (RGBA8).
  next.gbufferEmissiveTex = dev->create_texture_2d(w32, h32, 4, nullptr);
  if (next.gbufferEmissiveTex == 0U) {
    return fail_create(next, "failed to create G-Buffer emissive texture");
  }

  // G-Buffer depth (DEPTH24).
  next.gbufferDepthTex = dev->create_depth_texture(w32, h32);
  if (next.gbufferDepthTex == 0U) {
    return fail_create(next, "failed to create G-Buffer depth texture");
  }

  // G-Buffer MRT FBO (3 color + 1 depth).
  const std::uint32_t gbufferColors[] = {next.gbufferAlbedoTex,
                                         next.gbufferNormalTex,
                                         next.gbufferEmissiveTex};
  next.gbufferFbo =
      dev->create_framebuffer_mrt(gbufferColors, 3, next.gbufferDepthTex);
  if (next.gbufferFbo == 0U) {
    return fail_create(next, "failed to create G-Buffer framebuffer");
  }

  // Verify G-Buffer FBO completeness.
  dev->bind_framebuffer(next.gbufferFbo);
  if (!dev->check_framebuffer_complete()) {
    dev->bind_framebuffer(0U);
    return fail_create(next, "G-Buffer FBO is not complete");
  }
  dev->bind_framebuffer(0U);

  next.resources.gbufferAlbedo = PassResourceId{kGBufferAlbedoSlot};
  next.resources.gbufferNormal = PassResourceId{kGBufferNormalSlot};
  next.resources.gbufferEmissive = PassResourceId{kGBufferEmissiveSlot};
  next.resources.gbufferDepth = PassResourceId{kGBufferDepthSlot};

  // --- SSAO textures (R32F) ---
  next.ssaoTex = dev->create_texture_2d_r32f(w32, h32, nullptr);
  if (next.ssaoTex == 0U) {
    return fail_create(next, "failed to create SSAO texture");
  }

  next.ssaoFbo = dev->create_framebuffer(next.ssaoTex, 0U);
  if (next.ssaoFbo == 0U) {
    return fail_create(next, "failed to create SSAO framebuffer");
  }

  next.ssaoBlurTex = dev->create_texture_2d_r32f(w32, h32, nullptr);
  if (next.ssaoBlurTex == 0U) {
    return fail_create(next, "failed to create SSAO blur texture");
  }

  next.ssaoBlurFbo = dev->create_framebuffer(next.ssaoBlurTex, 0U);
  if (next.ssaoBlurFbo == 0U) {
    return fail_create(next, "failed to create SSAO blur framebuffer");
  }

  next.resources.ssaoTexture = PassResourceId{kSsaoTextureSlot};
  next.resources.ssaoBlurTexture = PassResourceId{kSsaoBlurTextureSlot};
  next.initialized = true;

  *outState = next;
  return true;
}

} // namespace

/// Initializes the owning system for pass resources.
bool initialize_pass_resources(int width, int height) noexcept {
  if (g_state.initialized) {
    return true;
  }

  if ((width <= 0) || (height <= 0)) {
    return false;
  }

  PassResourceState next{};
  if (!create_gpu_resources(&next, width, height)) {
    return false;
  }

  g_state = next;
  return true;
}

/// Shuts down the owning system for pass resources.
void shutdown_pass_resources() noexcept {
  if (!g_state.initialized) {
    return;
  }

  destroy_gpu_resources(g_state);
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

  PassResourceState next{};
  if (!create_gpu_resources(&next, width, height)) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to recreate pass resources on resize");
    return;
  }

  destroy_gpu_resources(g_state);
  g_state = next;
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
  if (resource.id == kSsaoTextureSlot) {
    return g_state.ssaoTex;
  }
  if (resource.id == kSsaoBlurTextureSlot) {
    return g_state.ssaoBlurTex;
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
  if (colorAttachment.id == kSsaoTextureSlot) {
    return g_state.ssaoFbo;
  }
  if (colorAttachment.id == kSsaoBlurTextureSlot) {
    return g_state.ssaoBlurFbo;
  }
  return 0U;
}

} // namespace engine::renderer
