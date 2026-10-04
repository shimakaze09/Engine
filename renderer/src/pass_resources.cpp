// Implements pass resources behavior for the Engine renderer system.

#include "engine/renderer/pass_resources.h"

#include <array>
#include <cstddef>
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
/// A resource id is its slot plus kViewStride times its view, so an id
/// names the view whose target it is.
constexpr std::uint32_t kViewStride = 16U;

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

std::array<PassResourceState, kMaxRenderViews> g_states{};

/// The state an id belongs to, and the id's slot; null for an id outside
/// every view.
PassResourceState *state_for(PassResourceId resource,
                             std::uint32_t *outSlot) noexcept {
  const std::uint32_t view = resource.id / kViewStride;
  if ((resource.id == 0U) || (view >= kMaxRenderViews)) {
    return nullptr;
  }
  *outSlot = resource.id % kViewStride;
  return &g_states[view];
}

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
bool create_gpu_resources(PassResourceState *outState, std::size_t view,
                          int width, int height) noexcept {
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
  // so a generated chain would hold stale data forever. Every
  // consumer samples them 1:1 with linear filtering.
  const auto makeTexture = [&](TextureFormat format,
                               bool blitDestination = false) noexcept {
    TextureDesc desc{};
    desc.blitDestination = blitDestination;
    desc.kind = TextureKind::Tex2D;
    desc.format = format;
    desc.width = w32;
    desc.height = h32;
    desc.filter = TextureFilter::Linear;
    desc.wrap = TextureWrap::ClampEdge;
    // R32F and depth: exact fetches/comparisons, and WebGL2 treats
    // these formats with linear filtering as incomplete (all-zero
    // samples), so they must stay point-sampled.
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

  // The deferred path seeds it from the G-buffer depth through copy_depth.
  next.sceneDepthTexture =
      makeTexture(TextureFormat::Depth24, /*blitDestination=*/true);
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

  next.resources.sceneColor = PassResourceId{
      kSceneColorSlot + (static_cast<std::uint32_t>(view) * kViewStride)};
  next.resources.sceneDepth = PassResourceId{
      kSceneDepthSlot + (static_cast<std::uint32_t>(view) * kViewStride)};
  next.resources.finalColor = PassResourceId{
      kFinalColorSlot + (static_cast<std::uint32_t>(view) * kViewStride)};

  next.gbufferAlbedoTex = makeTexture(TextureFormat::RGBA8);
  if (next.gbufferAlbedoTex == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create G-Buffer albedo texture");
  }

  next.gbufferNormalTex = makeTexture(TextureFormat::RGBA16F);
  if (next.gbufferNormalTex == kInvalidDeviceTexture) {
    return fail_create(next, "failed to create G-Buffer normal texture");
  }

  // HDR, as the forward path's scene target is: emission above 1 is what
  // bloom picks up, and an 8-bit target would clamp it on every deferred
  // surface while the same material glows when forward-shaded.
  next.gbufferEmissiveTex = makeTexture(TextureFormat::RGBA16F);
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

  next.resources.gbufferAlbedo = PassResourceId{
      kGBufferAlbedoSlot + (static_cast<std::uint32_t>(view) * kViewStride)};
  next.resources.gbufferNormal = PassResourceId{
      kGBufferNormalSlot + (static_cast<std::uint32_t>(view) * kViewStride)};
  next.resources.gbufferEmissive = PassResourceId{
      kGBufferEmissiveSlot + (static_cast<std::uint32_t>(view) * kViewStride)};
  next.resources.gbufferDepth = PassResourceId{
      kGBufferDepthSlot + (static_cast<std::uint32_t>(view) * kViewStride)};

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

  next.resources.ssaoTexture = PassResourceId{
      kSsaoTextureSlot + (static_cast<std::uint32_t>(view) * kViewStride)};
  next.resources.ssaoBlurTexture = PassResourceId{
      kSsaoBlurTextureSlot + (static_cast<std::uint32_t>(view) * kViewStride)};
  next.initialized = true;

  *outState = next;
  return true;
}

} // namespace

/// Initializes the owning system for pass resources.
bool initialize_pass_resources(int width, int height,
                               std::size_t view) noexcept {
  if (view >= kMaxRenderViews) {
    return false;
  }
  PassResourceState &state = g_states[view];
  if (state.initialized) {
    return true;
  }

  if ((width <= 0) || (height <= 0)) {
    return false;
  }

  PassResourceState next{};
  if (!create_gpu_resources(&next, view, width, height)) {
    return false;
  }

  state = next;
  return true;
}

/// Shuts down the owning system for pass resources: every view's targets.
void shutdown_pass_resources() noexcept {
  for (PassResourceState &state : g_states) {
    if (!state.initialized) {
      continue;
    }
    if (render_device() == nullptr) {
      core::log_message(core::LogLevel::Error, "pass_resources",
                        "shutdown without a render device leaks GPU targets");
    }
    destroy_gpu_resources(state);
    state = PassResourceState{};
  }
}

bool resize_pass_resources(int width, int height, std::size_t view) noexcept {
  if (view >= kMaxRenderViews) {
    return false;
  }
  PassResourceState &state = g_states[view];
  if (!state.initialized) {
    return false;
  }

  if ((width <= 0) || (height <= 0)) {
    return false;
  }

  if ((width == state.width) && (height == state.height)) {
    return true;
  }

  PassResourceState next{};
  if (!create_gpu_resources(&next, view, width, height)) {
    core::log_message(core::LogLevel::Error, "pass_resources",
                      "failed to recreate pass resources on resize — "
                      "keeping previous targets");
    return false;
  }

  destroy_gpu_resources(state);
  state = next;
  return true;
}

const PassResources &get_pass_resources(std::size_t view) noexcept {
  static const PassResources kNone{};
  return (view < kMaxRenderViews) ? g_states[view].resources : kNone;
}

DeviceTextureHandle pass_resource_texture(PassResourceId resource) noexcept {
  std::uint32_t slot = 0U;
  const PassResourceState *state = state_for(resource, &slot);
  if (state == nullptr) {
    return kInvalidDeviceTexture;
  }
  switch (slot) {
  case kSceneColorSlot:
    return state->sceneColorTexture;
  case kSceneDepthSlot:
    return state->sceneDepthTexture;
  case kFinalColorSlot:
    return state->finalColorTexture;
  case kGBufferAlbedoSlot:
    return state->gbufferAlbedoTex;
  case kGBufferNormalSlot:
    return state->gbufferNormalTex;
  case kGBufferEmissiveSlot:
    return state->gbufferEmissiveTex;
  case kGBufferDepthSlot:
    return state->gbufferDepthTex;
  case kSsaoTextureSlot:
    return state->ssaoTex;
  case kSsaoBlurTextureSlot:
    return state->ssaoBlurTex;
  default:
    return kInvalidDeviceTexture;
  }
}

RenderTargetHandle
pass_resource_target(PassResourceId colorAttachment) noexcept {
  std::uint32_t slot = 0U;
  const PassResourceState *state = state_for(colorAttachment, &slot);
  if (state == nullptr) {
    return RenderTargetHandle{};
  }
  switch (slot) {
  case kSceneColorSlot:
    return state->sceneTarget;
  case kFinalColorSlot:
    return state->finalTarget;
  case kGBufferAlbedoSlot:
    return state->gbufferTarget;
  case kSsaoTextureSlot:
    return state->ssaoTarget;
  case kSsaoBlurTextureSlot:
    return state->ssaoBlurTarget;
  default:
    return RenderTargetHandle{};
  }
}

} // namespace engine::renderer
