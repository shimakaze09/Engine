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

/// Destroys or releases the requested object, handle, or resource for bloom resources.
void destroy_bloom_resources(BackendState &b) noexcept {
  const auto *dev = render_device();
  if (dev == nullptr) {
    return;
  }
  for (int i = 0; i < BackendState::kBloomMipLevels; ++i) {
    if (b.bloomMipFbos[i] != 0U) {
      dev->destroy_framebuffer(b.bloomMipFbos[i]);
      b.bloomMipFbos[i] = 0U;
    }
    if (b.bloomMipTextures[i] != 0U) {
      dev->destroy_texture(b.bloomMipTextures[i]);
      b.bloomMipTextures[i] = 0U;
    }
  }
  b.bloomAllocatedWidth = 0;
  b.bloomAllocatedHeight = 0;
}

/// Handles ensure bloom resources.
void ensure_bloom_resources(BackendState &b, int width, int height) noexcept {
  if (b.bloomAllocatedWidth == width && b.bloomAllocatedHeight == height) {
    return;
  }
  destroy_bloom_resources(b);
  const auto *dev = render_device();
  int w = width / 2;
  int h = height / 2;
  for (int i = 0; i < BackendState::kBloomMipLevels; ++i) {
    if (w < 1) {
      w = 1;
    }
    if (h < 1) {
      h = 1;
    }
    b.bloomMipWidths[i] = w;
    b.bloomMipHeights[i] = h;
    b.bloomMipTextures[i] = dev->create_texture_2d_hdr(w, h, 4, nullptr);
    b.bloomMipFbos[i] = dev->create_framebuffer(b.bloomMipTextures[i], 0U);
    w /= 2;
    h /= 2;
  }
  b.bloomAllocatedWidth = width;
  b.bloomAllocatedHeight = height;
}

/// Destroys or releases the requested object, handle, or resource for luminance resources.
void destroy_luminance_resources(BackendState &b) noexcept {
  const auto *dev = render_device();
  if (dev == nullptr) {
    return;
  }
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if (b.lumMipFbos[i] != 0U) {
      dev->destroy_framebuffer(b.lumMipFbos[i]);
      b.lumMipFbos[i] = 0U;
    }
    if (b.lumMipTextures[i] != 0U) {
      dev->destroy_texture(b.lumMipTextures[i]);
      b.lumMipTextures[i] = 0U;
    }
  }
  b.lumAllocatedWidth = 0;
  b.lumAllocatedHeight = 0;
}

/// Handles ensure luminance resources.
void ensure_luminance_resources(BackendState &b, int width,
                                int height) noexcept {
  if (b.lumAllocatedWidth == width && b.lumAllocatedHeight == height) {
    return;
  }
  destroy_luminance_resources(b);
  const auto *dev = render_device();
  int w = width / 2;
  int h = height / 2;
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if (w < 1) {
      w = 1;
    }
    if (h < 1) {
      h = 1;
    }
    b.lumMipWidths[i] = w;
    b.lumMipHeights[i] = h;
    b.lumMipTextures[i] = dev->create_texture_2d_hdr(w, h, 4, nullptr);
    b.lumMipFbos[i] = dev->create_framebuffer(b.lumMipTextures[i], 0U);
    w /= 2;
    h /= 2;
  }
  b.lumAllocatedWidth = width;
  b.lumAllocatedHeight = height;
}

/// Handles generate ssao kernel.
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
std::uint32_t create_ssao_noise_texture() noexcept {
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
  return render_device()->create_texture_2d_hdr(4, 4, 4, noise);
}

} // namespace engine::renderer
