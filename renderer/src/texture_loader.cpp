// Implements texture loader behavior for the Engine renderer system.

#include "engine/renderer/texture_loader.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

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
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // conversion from 'int' to 'short'
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace engine::renderer {

namespace {

constexpr std::size_t kMaxTextureSlots = 512U;
constexpr std::size_t kMaxPathLen = 260U;
constexpr std::int32_t kHdrCubemapChannels = 3;
constexpr std::int32_t kMaxCubemapFaceSize = 4096;
constexpr double kPi = 3.14159265358979323846264338327950288;

/// Stores texture slot data used by the engine.
struct TextureSlot final {
  std::uint32_t gpuId = 0U;
  bool occupied = false;
  bool hdr = false;
  bool cubemap = false;
  std::array<char, kMaxPathLen> path{};
};

/// Stores texture system state data used by the engine.
struct TextureSystemState final {
  bool initialized = false;
  std::array<TextureSlot, kMaxTextureSlots> slots{};
};

TextureSystemState g_texState{};

/// Handles safe copy path.
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

/// Finds the matching object or resource for free texture slot.
std::size_t find_free_texture_slot() noexcept {
  // Slot 0 is reserved as the invalid handle.
  for (std::size_t i = 1U; i < kMaxTextureSlots; ++i) {
    if (!g_texState.slots[i].occupied) {
      return i;
    }
  }

  return 0U;
}

/// Handles lookup texture slot.
TextureSlot *lookup_texture_slot(TextureHandle handle) noexcept {
  if (!g_texState.initialized || handle == kInvalidTextureHandle) {
    return nullptr;
  }

  const std::uint32_t idx = handle.id;
  if ((idx >= kMaxTextureSlots) || !g_texState.slots[idx].occupied) {
    return nullptr;
  }

  return &g_texState.slots[idx];
}

/// Handles clamp int.
int clamp_int(int value, int minValue, int maxValue) noexcept {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

/// Handles clamp float.
float clamp_float(float value, float minValue, float maxValue) noexcept {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

/// Handles wrap index.
int wrap_index(int value, int count) noexcept {
  int result = value % count;
  if (result < 0) {
    result += count;
  }
  return result;
}

/// Stores vec3 data used by the engine.
struct Vec3 final {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

/// Clamps and fills settings into a safe runtime range.
Vec3 normalize(Vec3 value) noexcept {
  const float lenSq = value.x * value.x + value.y * value.y + value.z * value.z;
  if (lenSq <= 0.0F) {
    return {};
  }

  const float invLen = 1.0F / std::sqrt(lenSq);
  value.x *= invLen;
  value.y *= invLen;
  value.z *= invLen;
  return value;
}

/// Handles cube face direction.
Vec3 cube_face_direction(int face, float u, float v) noexcept {
  switch (face) {
  case 0:
    return normalize({1.0F, -v, -u});
  case 1:
    return normalize({-1.0F, -v, u});
  case 2:
    return normalize({u, 1.0F, v});
  case 3:
    return normalize({u, -1.0F, -v});
  case 4:
    return normalize({u, -v, 1.0F});
  default:
    return normalize({-u, -v, -1.0F});
  }
}

/// Handles sample equirect channel.
float sample_equirect_channel(const float *pixels, int width, int height,
                              double u, double v, int channel) noexcept {
  u -= std::floor(u);
  const double clampedV = (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);

  const double sourceX = u * static_cast<double>(width) - 0.5;
  const double sourceY = clampedV * static_cast<double>(height) - 0.5;
  const int x0 = static_cast<int>(std::floor(sourceX));
  const int y0 = static_cast<int>(std::floor(sourceY));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;
  const float tx = static_cast<float>(sourceX - static_cast<double>(x0));
  const float ty = static_cast<float>(sourceY - static_cast<double>(y0));

  const int wrappedX0 = wrap_index(x0, width);
  const int wrappedX1 = wrap_index(x1, width);
  const int clampedY0 = clamp_int(y0, 0, height - 1);
  const int clampedY1 = clamp_int(y1, 0, height - 1);

  const auto pixel_at = [pixels, width](int x, int y, int c) noexcept {
    const std::size_t idx =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(x)) *
            static_cast<std::size_t>(kHdrCubemapChannels) +
        static_cast<std::size_t>(c);
    return pixels[idx];
  };

  const float c00 = pixel_at(wrappedX0, clampedY0, channel);
  const float c10 = pixel_at(wrappedX1, clampedY0, channel);
  const float c01 = pixel_at(wrappedX0, clampedY1, channel);
  const float c11 = pixel_at(wrappedX1, clampedY1, channel);
  const float cx0 = c00 + (c10 - c00) * tx;
  const float cx1 = c01 + (c11 - c01) * tx;
  return cx0 + (cx1 - cx0) * ty;
}

/// Handles sample equirect direction.
void sample_equirect_direction(const float *pixels, int width, int height,
                               Vec3 direction, float *outRgb) noexcept {
  const double theta = std::atan2(static_cast<double>(direction.z),
                                  static_cast<double>(direction.x));
  const double u = (theta / (2.0 * kPi)) + 0.5;
  const double v =
      std::acos(static_cast<double>(clamp_float(direction.y, -1.0F, 1.0F))) /
      kPi;

  for (int channel = 0; channel < kHdrCubemapChannels; ++channel) {
    outRgb[channel] =
        sample_equirect_channel(pixels, width, height, u, v, channel);
  }
}

/// Handles allocate equirect cubemap faces.
bool allocate_equirect_cubemap_faces(
    const float *pixels, int width, int height, std::int32_t faceSize,
    std::array<std::unique_ptr<float[]>, 6> &faces) noexcept {
  if ((pixels == nullptr) || (width <= 0) || (height <= 0) || (faceSize <= 0) ||
      (faceSize > kMaxCubemapFaceSize)) {
    return false;
  }

  const auto faceSizeU = static_cast<std::size_t>(faceSize);
  const std::size_t maxSize = std::numeric_limits<std::size_t>::max();
  if (faceSizeU > (maxSize / faceSizeU) ||
      (faceSizeU * faceSizeU) >
          (maxSize / static_cast<std::size_t>(kHdrCubemapChannels))) {
    return false;
  }

  const std::size_t texelCount = faceSizeU * faceSizeU;
  const std::size_t floatCount =
      texelCount * static_cast<std::size_t>(kHdrCubemapChannels);

  for (auto &face : faces) {
    face.reset(new (std::nothrow) float[floatCount]);
    if (face == nullptr) {
      return false;
    }
  }

  for (int face = 0; face < 6; ++face) {
    float *dst = faces[static_cast<std::size_t>(face)].get();
    for (std::int32_t y = 0; y < faceSize; ++y) {
      const float v = (2.0F * (static_cast<float>(y) + 0.5F) /
                       static_cast<float>(faceSize)) -
                      1.0F;
      for (std::int32_t x = 0; x < faceSize; ++x) {
        const float u = (2.0F * (static_cast<float>(x) + 0.5F) /
                         static_cast<float>(faceSize)) -
                        1.0F;
        const Vec3 dir = cube_face_direction(face, u, v);
        const std::size_t dstIdx =
            (static_cast<std::size_t>(y) * faceSizeU +
             static_cast<std::size_t>(x)) *
            static_cast<std::size_t>(kHdrCubemapChannels);
        sample_equirect_direction(pixels, width, height, dir, &dst[dstIdx]);
      }
    }
  }

  return true;
}

} // namespace

/// Initializes the owning system for texture system.
bool initialize_texture_system() noexcept {
  if (g_texState.initialized) {
    return true;
  }

  g_texState = TextureSystemState{};
  g_texState.initialized = true;
  return true;
}

/// Shuts down the owning system for texture system.
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

/// Loads the requested resource for texture.
TextureHandle load_texture(const char *virtualPath) noexcept {
  if ((virtualPath == nullptr) || !g_texState.initialized) {
    return kInvalidTextureHandle;
  }

  const std::size_t freeSlot = find_free_texture_slot();
  if (freeSlot == 0U) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "texture registry full");
    return kInvalidTextureHandle;
  }

  // Read file via VFS.
  void *fileData = nullptr;
  std::size_t fileSize = 0U;
  if (!core::vfs_read_binary(virtualPath, &fileData, &fileSize)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to read texture file");
    return kInvalidTextureHandle;
  }

  if ((fileData == nullptr) || (fileSize == 0U)) {
    if (fileData != nullptr) {
      core::vfs_free(fileData);
    }
    core::log_message(core::LogLevel::Error, "renderer",
                      "texture file is empty");
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
      core::log_message(core::LogLevel::Error, "renderer",
                        "failed to decode HDR texture");
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
      core::log_message(core::LogLevel::Error, "renderer",
                        "failed to decode texture");
      return kInvalidTextureHandle;
    }

    const RenderDevice *dev = render_device();
    if (dev != nullptr && dev->create_texture_2d != nullptr) {
      gpuId = dev->create_texture_2d(width, height, channels, pixels);
    }
    stbi_image_free(pixels);
  }

  if (gpuId == 0U) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to create GPU texture");
    return kInvalidTextureHandle;
  }

  TextureSlot &slot = g_texState.slots[freeSlot];
  slot.gpuId = gpuId;
  slot.occupied = true;
  slot.hdr = isHdr;
  slot.cubemap = false;
  safe_copy_path(slot.path.data(), slot.path.size(), virtualPath);

  TextureHandle handle{};
  handle.id = static_cast<std::uint32_t>(freeSlot);
  return handle;
}

/// Loads the requested resource for hdr equirect cubemap.
TextureHandle load_hdr_equirect_cubemap(const char *virtualPath,
                                        std::int32_t faceSize) noexcept {
  if ((virtualPath == nullptr) || !g_texState.initialized || (faceSize <= 0) ||
      (faceSize > kMaxCubemapFaceSize)) {
    return kInvalidTextureHandle;
  }

  const std::size_t freeSlot = find_free_texture_slot();
  if (freeSlot == 0U) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "texture registry full");
    return kInvalidTextureHandle;
  }

  void *fileData = nullptr;
  std::size_t fileSize = 0U;
  if (!core::vfs_read_binary(virtualPath, &fileData, &fileSize)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to read HDR equirect texture file");
    return kInvalidTextureHandle;
  }

  if ((fileData == nullptr) || (fileSize == 0U)) {
    if (fileData != nullptr) {
      core::vfs_free(fileData);
    }
    core::log_message(core::LogLevel::Error, "renderer",
                      "HDR equirect texture file is empty");
    return kInvalidTextureHandle;
  }

  const auto *fileBytes = static_cast<const unsigned char *>(fileData);
  if (stbi_is_hdr_from_memory(fileBytes, static_cast<int>(fileSize)) == 0) {
    core::vfs_free(fileData);
    core::log_message(core::LogLevel::Error, "renderer",
                      "equirect cubemap import requires an HDR texture");
    return kInvalidTextureHandle;
  }

  int width = 0;
  int height = 0;
  int sourceChannels = 0;
  float *pixels =
      stbi_loadf_from_memory(fileBytes, static_cast<int>(fileSize), &width,
                             &height, &sourceChannels, kHdrCubemapChannels);
  core::vfs_free(fileData);

  if ((pixels == nullptr) || (width <= 0) || (height <= 0)) {
    if (pixels != nullptr) {
      stbi_image_free(pixels);
    }
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to decode HDR equirect texture");
    return kInvalidTextureHandle;
  }

  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->create_cubemap_hdr == nullptr)) {
    stbi_image_free(pixels);
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to create HDR cubemap texture");
    return kInvalidTextureHandle;
  }

  std::array<std::unique_ptr<float[]>, 6> faces{};
  if (!allocate_equirect_cubemap_faces(pixels, width, height, faceSize,
                                       faces)) {
    stbi_image_free(pixels);
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to convert HDR equirect texture to cubemap");
    return kInvalidTextureHandle;
  }
  stbi_image_free(pixels);

  std::array<const float *, 6> facePixels{};
  for (std::size_t i = 0U; i < facePixels.size(); ++i) {
    facePixels[i] = faces[i].get();
  }

  const std::uint32_t gpuId =
      dev->create_cubemap_hdr(faceSize, kHdrCubemapChannels, facePixels.data());
  if (gpuId == 0U) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to upload HDR cubemap texture");
    return kInvalidTextureHandle;
  }

  TextureSlot &slot = g_texState.slots[freeSlot];
  slot.gpuId = gpuId;
  slot.occupied = true;
  slot.hdr = true;
  slot.cubemap = true;
  safe_copy_path(slot.path.data(), slot.path.size(), virtualPath);

  TextureHandle handle{};
  handle.id = static_cast<std::uint32_t>(freeSlot);
  return handle;
}

/// Handles unload texture.
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

/// Handles texture gpu id.
std::uint32_t texture_gpu_id(TextureHandle handle) noexcept {
  const TextureSlot *slot = lookup_texture_slot(handle);
  if (slot == nullptr) {
    return 0U;
  }

  return slot->gpuId;
}

/// Returns whether is texture hdr.
bool is_texture_hdr(TextureHandle handle) noexcept {
  const TextureSlot *slot = lookup_texture_slot(handle);
  if (slot == nullptr) {
    return false;
  }

  return slot->hdr;
}

/// Returns whether is texture cubemap.
bool is_texture_cubemap(TextureHandle handle) noexcept {
  const TextureSlot *slot = lookup_texture_slot(handle);
  if (slot == nullptr) {
    return false;
  }

  return slot->cubemap;
}

} // namespace engine::renderer
