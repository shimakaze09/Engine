// Implements size-tracked GPU resources for the renderer post-processing
// passes: bloom and luminance mip chains plus SSAO sampling data.
// Split out of command_buffer.cpp (REVIEW_FINDINGS A1).

#include "command_buffer_post_resources.h"

#include "command_buffer_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/light_culling.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/post_process_stack.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/shadow_map.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

namespace {

/// RGBA16F post-chain texture with the loader-default linear sampling.
DeviceTextureHandle create_post_chain_texture(const RenderDevice *dev, int w,
                                              int h) noexcept {
  if ((dev == nullptr) || (dev->create_texture == nullptr)) {
    return kInvalidDeviceTexture;
  }
  TextureDesc desc{};
  desc.kind = TextureKind::Tex2D;
  desc.format = TextureFormat::RGBA16F;
  desc.width = w;
  desc.height = h;
  desc.filter = TextureFilter::Linear;
  desc.wrap = TextureWrap::ClampEdge;
  return dev->create_texture(desc);
}

/// Color-only render target over one post-chain texture.
RenderTargetHandle create_post_chain_target(const RenderDevice *dev,
                                            DeviceTextureHandle color) noexcept {
  if ((dev == nullptr) || (dev->create_render_target == nullptr) ||
      (color == kInvalidDeviceTexture)) {
    return RenderTargetHandle{};
  }
  RenderTargetDesc desc{};
  desc.colorCount = 1U;
  desc.colors[0].texture = color;
  return dev->create_render_target(desc);
}

} // namespace

/// Destroys or releases the requested object, handle, or resource for bloom resources.
void destroy_bloom_resources(BackendState &b) noexcept {
  const auto *dev = render_device();
  if (dev == nullptr) {
    return;
  }
  for (int i = 0; i < BackendState::kBloomMipLevels; ++i) {
    if (b.view().bloomMipTargets[i].value != 0U) {
      dev->destroy_render_target(b.view().bloomMipTargets[i]);
      b.view().bloomMipTargets[i] = RenderTargetHandle{};
    }
    if (b.view().bloomMipTextures[i] != kInvalidDeviceTexture) {
      dev->destroy_texture(b.view().bloomMipTextures[i]);
      b.view().bloomMipTextures[i] = kInvalidDeviceTexture;
    }
  }
  b.view().bloomAllocatedWidth = 0;
  b.view().bloomAllocatedHeight = 0;
}

bool ensure_bloom_resources(BackendState &b, int width, int height) noexcept {
  if (b.view().bloomAllocatedWidth == width &&
      b.view().bloomAllocatedHeight == height) {
    return b.view().bloomMipTargets[0].value != 0U;
  }
  destroy_bloom_resources(b);
  const auto *dev = render_device();
  if (dev == nullptr) {
    return false;
  }
  int w = width / 2;
  int h = height / 2;
  bool complete = true;
  for (int i = 0; i < BackendState::kBloomMipLevels; ++i) {
    if (w < 1) {
      w = 1;
    }
    if (h < 1) {
      h = 1;
    }
    b.view().bloomMipWidths[i] = w;
    b.view().bloomMipHeights[i] = h;
    b.view().bloomMipTextures[i] = create_post_chain_texture(dev, w, h);
    b.view().bloomMipTargets[i] =
        create_post_chain_target(dev, b.view().bloomMipTextures[i]);
    if (b.view().bloomMipTargets[i].value == 0U) {
      complete = false;
      break;
    }
    w /= 2;
    h /= 2;
  }
  if (!complete) {
    destroy_bloom_resources(b);
    core::log_message(core::LogLevel::Error, "renderer",
                      "bloom mip chain creation failed; bloom stays off "
                      "until the drawable size changes");
  }
  b.view().bloomAllocatedWidth = width;
  b.view().bloomAllocatedHeight = height;
  return complete;
}

/// Destroys or releases the requested object, handle, or resource for luminance resources.
void destroy_luminance_resources(BackendState &b) noexcept {
  const auto *dev = render_device();
  if (dev == nullptr) {
    return;
  }
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if (b.view().lumMipTargets[i].value != 0U) {
      dev->destroy_render_target(b.view().lumMipTargets[i]);
      b.view().lumMipTargets[i] = RenderTargetHandle{};
    }
    if (b.view().lumMipTextures[i] != kInvalidDeviceTexture) {
      dev->destroy_texture(b.view().lumMipTextures[i]);
      b.view().lumMipTextures[i] = kInvalidDeviceTexture;
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (b.view().exposureTargets[i].value != 0U) {
      dev->destroy_render_target(b.view().exposureTargets[i]);
      b.view().exposureTargets[i] = RenderTargetHandle{};
    }
    if (b.view().exposureTextures[i] != kInvalidDeviceTexture) {
      dev->destroy_texture(b.view().exposureTextures[i]);
      b.view().exposureTextures[i] = kInvalidDeviceTexture;
    }
  }
  b.view().exposureCurrent = 0;
  b.view().exposureValid = false;
  b.view().lumAllocatedWidth = 0;
  b.view().lumAllocatedHeight = 0;
}

bool ensure_luminance_resources(BackendState &b, int width,
                                int height) noexcept {
  if (b.view().lumAllocatedWidth == width &&
      b.view().lumAllocatedHeight == height) {
    return b.view().lumMipTargets[0].value != 0U;
  }
  destroy_luminance_resources(b);
  const auto *dev = render_device();
  if (dev == nullptr) {
    return false;
  }
  int w = width / 2;
  int h = height / 2;
  bool complete = true;
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if (w < 1) {
      w = 1;
    }
    if (h < 1) {
      h = 1;
    }
    b.view().lumMipWidths[i] = w;
    b.view().lumMipHeights[i] = h;
    b.view().lumMipTextures[i] = create_post_chain_texture(dev, w, h);
    b.view().lumMipTargets[i] =
        create_post_chain_target(dev, b.view().lumMipTextures[i]);
    if (b.view().lumMipTargets[i].value == 0U) {
      complete = false;
      break;
    }
    w /= 2;
    h /= 2;
  }
  for (int i = 0; complete && (i < 2); ++i) {
    b.view().exposureTextures[i] = create_post_chain_texture(dev, 1, 1);
    b.view().exposureTargets[i] =
        create_post_chain_target(dev, b.view().exposureTextures[i]);
    complete = b.view().exposureTargets[i].value != 0U;
  }
  if (!complete) {
    destroy_luminance_resources(b);
    core::log_message(core::LogLevel::Error, "renderer",
                      "luminance mip chain creation failed; auto exposure "
                      "stays off until the drawable size changes");
  }
  b.view().lumAllocatedWidth = width;
  b.view().lumAllocatedHeight = height;
  return complete;
}

void generate_ssao_kernel(float *kernel, int count) noexcept {
  unsigned int seed = 12345U;
  auto nextFloat = [&seed]() -> float {
    seed = seed * 1103515245U + 12345U;
    return static_cast<float>((seed >> 16) & 0x7FFF) / 32767.0F;
  };
  for (int i = 0; i < count; ++i) {
    float x = nextFloat() * 2.0F - 1.0F;
    float y = nextFloat() * 2.0F - 1.0F;
    float z = nextFloat();
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 0.001F) {
      x = 0.0F;
      y = 0.0F;
      z = 1.0F;
      len = 1.0F;
    }
    x /= len;
    y /= len;
    z /= len;
    float scale = static_cast<float>(i) / static_cast<float>(count);
    scale = 0.1F + 0.9F * scale * scale;
    kernel[i * 3 + 0] = x * scale;
    kernel[i * 3 + 1] = y * scale;
    kernel[i * 3 + 2] = z * scale;
  }
}

/// Creates a new object, handle, or resource for ssao noise texture.
DeviceTextureHandle create_ssao_noise_texture() noexcept {
  float noise[16 * 4] = {};
  unsigned int seed = 54321U;
  auto nextFloat = [&seed]() -> float {
    seed = seed * 1103515245U + 12345U;
    return static_cast<float>((seed >> 16) & 0x7FFF) / 32767.0F;
  };
  for (int i = 0; i < 16; ++i) {
    noise[i * 4 + 0] = nextFloat() * 2.0F - 1.0F;
    noise[i * 4 + 1] = nextFloat() * 2.0F - 1.0F;
    noise[i * 4 + 2] = 0.0F;
    noise[i * 4 + 3] = 0.0F;
  }
  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->create_texture == nullptr)) {
    return kInvalidDeviceTexture;
  }
  TextureDesc desc{};
  desc.kind = TextureKind::Tex2D;
  desc.format = TextureFormat::RGBA16F;
  desc.width = 4;
  desc.height = 4;
  desc.filter = TextureFilter::Linear;
  // The one post-chain texture that has to repeat: the SSAO pass tiles
  // these sixteen rotation vectors across the screen, sampling at
  // uv * (drawable / 4), so that neighbouring pixels turn the kernel
  // differently and the blur averages the pattern out. Clamped, every
  // pixel past the first four reads the same edge texel and the whole
  // screen shares one kernel orientation.
  desc.wrap = TextureWrap::Repeat;
  desc.pixelData = TexelData::F32;
  desc.pixels = noise;
  return dev->create_texture(desc);
}

} // namespace engine::renderer
