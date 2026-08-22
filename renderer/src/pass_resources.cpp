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

  DeviceTextureHandle sceneColorTexture{};
  DeviceTextureHandle sceneDepthTexture{};
  RenderTargetHandle sceneTarget{};

  DeviceTextureHandle finalColorTexture{};
  RenderTargetHandle finalTarget{};

  // G-Buffer textures (deferred path).
  DeviceTextureHandle gbufferAlbedoTex{};   // RGBA8 — albedo.rgb + metallic.a
  DeviceTextureHandle gbufferNormalTex{};   // RGBA16F — normal.xyz+roughness.a
  DeviceTextureHandle gbufferEmissiveTex{}; // RGBA8 — emissive.rgb + AO.a
  DeviceTextureHandle gbufferDepthTex{};    // DEPTH24
  RenderTargetHandle gbufferTarget{};

  // SSAO textures.
  DeviceTextureHandle ssaoTex{};     // R32F — raw AO
  RenderTargetHandle ssaoTarget{};
  DeviceTextureHandle ssaoBlurTex{}; // R32F — blurred AO
  RenderTargetHandle ssaoBlurTarget{};

  PassResources resources{};
};

PassResourceState g_state{};

/// Destroys or releases the requested object, handle, or resource for gpu resources.
void destroy_gpu_resources(PassResourceState &state) noexcept {
  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->destroy_texture == nullptr) ||
      (dev->destroy_render_target == nullptr)) {
    return;
  }

  const auto destroyTarget = [dev](RenderTargetHandle &target) noexcept {
    if (target.value != 0U) {
      dev->destroy_render_target(target);
      target = RenderTargetHandle{};
    }
  };
  const auto destroyTexture = [dev](DeviceTextureHandle &texture) noexcept {
    if (texture != kInvalidDeviceTexture) {
      dev->destroy_texture(texture);
      texture = kInvalidDeviceTexture;
    }
  };

  destroyTarget(state.ssaoBlurTarget);
  destroyTexture(state.ssaoBlurTex);
  destroyTarget(state.ssaoTarget);
  destroyTexture(state.ssaoTex);

  destroyTarget(state.gbufferTarget);
  destroyTexture(state.gbufferDepthTex);
  destroyTexture(state.gbufferEmissiveTex);
  destroyTexture(state.gbufferNormalTex);
  destroyTexture(state.gbufferAlbedoTex);

  destroyTarget(state.finalTarget);
  destroyTexture(state.finalColorTexture);
  destroyTarget(state.sceneTarget);
  destroyTexture(state.sceneColorTexture);
  destroyTexture(state.sceneDepthTexture);
}

bool fail_create(PassResourceState &state, const char *message) noexcept {
  core::log_message(core::LogLevel::Error, "pass_resources", message);
  destroy_gpu_resources(state);
  state = PassResourceState{};
  return false;
}

/// Creates every pass render target. Layout: scene RGBA16F + DEPTH24,
/// final RGBA8 LDR (tonemapped editor-viewport output, no depth);
/// G-Buffer RT0 albedo RGBA8, RT1 normals+roughness RGBA16F, RT2
/// emissive+AO RGBA8, DEPTH24, bound as one MRT FBO; SSAO R32F.
bool create_gpu_resources(PassResourceState *outState, int width,
                          int height) noexcept {
  if (outState == nullptr) {
    return false;
  }

  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->create_texture == nullptr) ||
      (dev->create_render_target == nullptr)) {
    return false;
  }

  PassResourceState next{};
  next.width = width;
  next.height = height;

  const auto w32 = static_cast<std::int32_t>(width);
  const auto h32 = static_cast<std::int32_t>(height);

  // Render-target textures are single-level: only mip 0 is ever rendered,
  // so a generated chain would hold stale data forever (issue #229). Every
  // consumer samples them 1:1 with linear filtering.
  const auto makeTexture = [&](TextureFormat format) noexcept {
    TextureDesc desc{};
    desc.kind = TextureKind::Tex2D;
    desc.format = format;
    desc.width = w32;
    desc.height = h32;
    desc.filter = TextureFilter::Linear;
    desc.wrap = TextureWrap::Repeat;
    // R32F and depth: exact fetches/comparisons, and WebGL2 treats
    // these formats with linear filtering as incomplete (all-zero
    // samples — #293), so they must stay point-sampled.
    if ((format == TextureFormat::R32F) ||
        (format == TextureFormat::Depth24)) {
      desc.filter = TextureFilter::Nearest;
      desc.wrap = TextureWrap::ClampEdge;
    }
    return dev->create_texture(desc);
  };
  const auto makeColorTarget = [&](DeviceTextureHandle color,
                                   DeviceTextureHandle depth) noexcept {
    RenderTargetDesc desc{};
    desc.colorCount = 1U;
    desc.colors[0].texture = color;
    desc.depth.texture = depth;
    return dev->create_render_target(desc);
  };

  next.sceneColorTexture = makeTexture(TextureFormat::RGBA16F);
  if (next.sceneColorTexture == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create scene color texture");
  }

  next.sceneDepthTexture = makeTexture(TextureFormat::Depth24);
  if (next.sceneDepthTexture == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create scene depth texture");
  }

  next.sceneTarget =
      makeColorTarget(next.sceneColorTexture, next.sceneDepthTexture);
  if (next.sceneTarget.value == 0U) {
    return fail_create(next, "failed to create scene render target");
  }

  next.finalColorTexture = makeTexture(TextureFormat::RGBA8);
  if (next.finalColorTexture == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create final color texture");
  }

  next.finalTarget =
      makeColorTarget(next.finalColorTexture, kInvalidDeviceTexture);
  if (next.finalTarget.value == 0U) {
    return fail_create(next, "failed to create final render target");
  }

  next.resources.sceneColor = PassResourceId{kSceneColorSlot};
  next.resources.sceneDepth = PassResourceId{kSceneDepthSlot};
  next.resources.finalColor = PassResourceId{kFinalColorSlot};

  next.gbufferAlbedoTex = makeTexture(TextureFormat::RGBA8);
  if (next.gbufferAlbedoTex == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create G-Buffer albedo texture");
  }

  next.gbufferNormalTex = makeTexture(TextureFormat::RGBA16F);
  if (next.gbufferNormalTex == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create G-Buffer normal texture");
  }

  next.gbufferEmissiveTex = makeTexture(TextureFormat::RGBA8);
  if (next.gbufferEmissiveTex == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create G-Buffer emissive texture");
  }

  next.gbufferDepthTex = makeTexture(TextureFormat::Depth24);
  if (next.gbufferDepthTex == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create G-Buffer depth texture");
  }

  {
    RenderTargetDesc gbufferDesc{};
    gbufferDesc.colorCount = 3U;
    gbufferDesc.colors[0].texture = next.gbufferAlbedoTex;
    gbufferDesc.colors[1].texture = next.gbufferNormalTex;
    gbufferDesc.colors[2].texture = next.gbufferEmissiveTex;
    gbufferDesc.depth.texture = next.gbufferDepthTex;
    next.gbufferTarget = dev->create_render_target(gbufferDesc);
  }
  if (next.gbufferTarget.value == 0U) {
    return fail_create(next, "failed to create G-Buffer render target");
  }

  next.resources.gbufferAlbedo = PassResourceId{kGBufferAlbedoSlot};
  next.resources.gbufferNormal = PassResourceId{kGBufferNormalSlot};
  next.resources.gbufferEmissive = PassResourceId{kGBufferEmissiveSlot};
  next.resources.gbufferDepth = PassResourceId{kGBufferDepthSlot};

  next.ssaoTex = makeTexture(TextureFormat::R32F);
  if (next.ssaoTex == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create SSAO texture");
  }

  next.ssaoTarget = makeColorTarget(next.ssaoTex, kInvalidDeviceTexture);
  if (next.ssaoTarget.value == 0U) {
    return fail_create(next, "failed to create SSAO render target");
  }

  next.ssaoBlurTex = makeTexture(TextureFormat::R32F);
  if (next.ssaoBlurTex == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create SSAO blur texture");
  }

  next.ssaoBlurTarget =
      makeColorTarget(next.ssaoBlurTex, kInvalidDeviceTexture);
  if (next.ssaoBlurTarget.value == 0U) {
    return fail_create(next, "failed to create SSAO blur render target");
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

  if (render_device() == nullptr) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "shutdown without a render device leaks GPU targets");
  }
  destroy_gpu_resources(g_state);
  g_state = PassResourceState{};
}

bool resize_pass_resources(int width, int height) noexcept {
  if (!g_state.initialized) {
    return false;
  }

  if ((width <= 0) || (height <= 0)) {
    return false;
  }

  if ((width == g_state.width) && (height == g_state.height)) {
    return true;
  }

  PassResourceState next{};
  if (!create_gpu_resources(&next, width, height)) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to recreate pass resources on resize — "
                      "keeping previous targets");
    return false;
  }

  destroy_gpu_resources(g_state);
  g_state = next;
  return true;
}

const PassResources &get_pass_resources() noexcept { return g_state.resources; }

DeviceTextureHandle pass_resource_texture(PassResourceId resource) noexcept {
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
  return kInvalidDeviceTexture;
}

RenderTargetHandle
pass_resource_target(PassResourceId colorAttachment) noexcept {
  if (colorAttachment.id == kSceneColorSlot) {
    return g_state.sceneTarget;
  }
  if (colorAttachment.id == kFinalColorSlot) {
    return g_state.finalTarget;
  }
  if (colorAttachment.id == kGBufferAlbedoSlot) {
    return g_state.gbufferTarget;
  }
  if (colorAttachment.id == kSsaoTextureSlot) {
    return g_state.ssaoTarget;
  }
  if (colorAttachment.id == kSsaoBlurTextureSlot) {
    return g_state.ssaoBlurTarget;
  }
  return RenderTargetHandle{};
}

} // namespace engine::renderer
