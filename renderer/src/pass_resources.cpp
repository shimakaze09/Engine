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

struct PassResourceState final {
  bool initialized = false;
  int width = 0;
  int height = 0;

  std::uint32_t sceneColorTexture = 0U;
  std::uint32_t sceneDepthTexture = 0U;
  std::uint32_t sceneFbo = 0U;

  PassResources resources{};
};

PassResourceState g_state{};

void destroy_gpu_resources() noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
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
                                 static_cast<std::int32_t>(height),
                                 4,
                                 nullptr);
  if (g_state.sceneColorTexture == 0U) {
    core::log_message(core::LogLevel::Error,
                      "pass_resources",
                      "failed to create scene color texture");
    return false;
  }

  // Scene depth: DEPTH24.
  g_state.sceneDepthTexture = dev->create_depth_texture(
      static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
  if (g_state.sceneDepthTexture == 0U) {
    core::log_message(core::LogLevel::Error,
                      "pass_resources",
                      "failed to create scene depth texture");
    dev->destroy_texture(g_state.sceneColorTexture);
    g_state.sceneColorTexture = 0U;
    return false;
  }

  // Scene FBO.
  g_state.sceneFbo = dev->create_framebuffer(g_state.sceneColorTexture,
                                             g_state.sceneDepthTexture);
  if (g_state.sceneFbo == 0U) {
    core::log_message(core::LogLevel::Error,
                      "pass_resources",
                      "failed to create scene framebuffer");
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
    core::log_message(core::LogLevel::Error,
                      "pass_resources",
                      "failed to recreate pass resources on resize");
    g_state.initialized = false;
  }
}

const PassResources &get_pass_resources() noexcept {
  return g_state.resources;
}

std::uint32_t pass_resource_gpu_texture(PassResourceId resource) noexcept {
  if (resource.id == kSceneColorSlot) {
    return g_state.sceneColorTexture;
  }
  if (resource.id == kSceneDepthSlot) {
    return g_state.sceneDepthTexture;
  }
  return 0U;
}

std::uint32_t
pass_resource_framebuffer(PassResourceId colorAttachment) noexcept {
  if (colorAttachment.id == kSceneColorSlot) {
    return g_state.sceneFbo;
  }
  // Final color = back buffer (FBO 0).
  if (colorAttachment.id == kFinalColorSlot) {
    return 0U;
  }
  return 0U;
}

} // namespace engine::renderer
