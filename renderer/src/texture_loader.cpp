#include "engine/renderer/texture_loader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"
#include "engine/renderer/render_device.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wcomma"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wcast-qual"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace engine::renderer {

namespace {

constexpr std::size_t kMaxTextureSlots = 512U;
constexpr std::size_t kMaxPathLen = 260U;

struct TextureSlot final {
  std::uint32_t gpuId = 0U;
  bool occupied = false;
  bool hdr = false;
  std::array<char, kMaxPathLen> path{};
};

struct TextureSystemState final {
  bool initialized = false;
  std::array<TextureSlot, kMaxTextureSlots> slots{};
};

TextureSystemState g_texState{};

void safe_copy_path(char *dst, std::size_t dstSize, const char *src) noexcept {
  if ((dst == nullptr) || (src == nullptr) || (dstSize == 0U)) {
    return;
  }

  std::size_t i = 0U;
  while ((i + 1U) < dstSize && src[i] != '\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

} // namespace

bool initialize_texture_system() noexcept {
  if (g_texState.initialized) {
    return true;
  }

  g_texState = TextureSystemState{};
  g_texState.initialized = true;
  return true;
}

void shutdown_texture_system() noexcept {
  if (!g_texState.initialized) {
    return;
  }

  const RenderDevice *dev = render_device();
  for (std::size_t i = 0U; i < kMaxTextureSlots; ++i) {
    if (g_texState.slots[i].occupied && g_texState.slots[i].gpuId != 0U) {
      if (dev != nullptr) {
        dev->destroy_texture(g_texState.slots[i].gpuId);
      }
    }
    g_texState.slots[i] = TextureSlot{};
  }

  g_texState.initialized = false;
}

TextureHandle load_texture(const char *virtualPath) noexcept {
  if ((virtualPath == nullptr) || !g_texState.initialized) {
    return kInvalidTextureHandle;
  }

  // Find free slot (skip 0 — reserved as invalid).
  std::size_t freeSlot = 0U;
  for (std::size_t i = 1U; i < kMaxTextureSlots; ++i) {
    if (!g_texState.slots[i].occupied) {
      freeSlot = i;
      break;
    }
  }

  if (freeSlot == 0U) {
    core::log_message(
        core::LogLevel::Error, "renderer", "texture registry full");
    return kInvalidTextureHandle;
  }

  // Read file via VFS.
  void *fileData = nullptr;
  std::size_t fileSize = 0U;
  if (!core::vfs_read_binary(virtualPath, &fileData, &fileSize)) {
    core::log_message(
        core::LogLevel::Error, "renderer", "failed to read texture file");
    return kInvalidTextureHandle;
  }

  if ((fileData == nullptr) || (fileSize == 0U)) {
    core::log_message(
        core::LogLevel::Error, "renderer", "texture file is empty");
    return kInvalidTextureHandle;
  }

  const auto *fileBytes = static_cast<const unsigned char *>(fileData);
  int width = 0;
  int height = 0;
  int channels = 0;
  bool isHdr = false;
  std::uint32_t gpuId = 0U;

  if (stbi_is_hdr_from_memory(fileBytes, static_cast<int>(fileSize)) != 0) {
    // HDR path.
    float *pixels = stbi_loadf_from_memory(
        fileBytes, static_cast<int>(fileSize), &width, &height, &channels, 0);
    core::vfs_free(fileData);

    if (pixels == nullptr) {
      core::log_message(
          core::LogLevel::Error, "renderer", "failed to decode HDR texture");
      return kInvalidTextureHandle;
    }

    const RenderDevice *dev = render_device();
    if (dev != nullptr && dev->create_texture_2d_hdr != nullptr) {
      gpuId = dev->create_texture_2d_hdr(width, height, channels, pixels);
    }
    stbi_image_free(pixels);
    isHdr = true;
  } else {
    // LDR path.
    unsigned char *pixels = stbi_load_from_memory(
        fileBytes, static_cast<int>(fileSize), &width, &height, &channels, 0);
    core::vfs_free(fileData);

    if (pixels == nullptr) {
      core::log_message(
          core::LogLevel::Error, "renderer", "failed to decode texture");
      return kInvalidTextureHandle;
    }

    const RenderDevice *dev = render_device();
    if (dev != nullptr && dev->create_texture_2d != nullptr) {
      gpuId = dev->create_texture_2d(width, height, channels, pixels);
    }
    stbi_image_free(pixels);
  }

  if (gpuId == 0U) {
    core::log_message(
        core::LogLevel::Error, "renderer", "failed to create GPU texture");
    return kInvalidTextureHandle;
  }

  TextureSlot &slot = g_texState.slots[freeSlot];
  slot.gpuId = gpuId;
  slot.occupied = true;
  slot.hdr = isHdr;
  safe_copy_path(slot.path.data(), slot.path.size(), virtualPath);

  TextureHandle handle{};
  handle.id = static_cast<std::uint32_t>(freeSlot);
  return handle;
}

void unload_texture(TextureHandle handle) noexcept {
  if (!g_texState.initialized || handle == kInvalidTextureHandle) {
    return;
  }

  const std::uint32_t idx = handle.id;
  if (idx >= kMaxTextureSlots || !g_texState.slots[idx].occupied) {
    return;
  }

  const RenderDevice *dev = render_device();
  if (dev != nullptr && g_texState.slots[idx].gpuId != 0U) {
    dev->destroy_texture(g_texState.slots[idx].gpuId);
  }

  g_texState.slots[idx] = TextureSlot{};
}

std::uint32_t texture_gpu_id(TextureHandle handle) noexcept {
  if (!g_texState.initialized || handle == kInvalidTextureHandle) {
    return 0U;
  }

  const std::uint32_t idx = handle.id;
  if (idx >= kMaxTextureSlots || !g_texState.slots[idx].occupied) {
    return 0U;
  }

  return g_texState.slots[idx].gpuId;
}

bool is_texture_hdr(TextureHandle handle) noexcept {
  if (!g_texState.initialized || handle == kInvalidTextureHandle) {
    return false;
  }

  const std::uint32_t idx = handle.id;
  if (idx >= kMaxTextureSlots || !g_texState.slots[idx].occupied) {
    return false;
  }

  return g_texState.slots[idx].hdr;
}

} // namespace engine::renderer
