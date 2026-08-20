// Implements texture loader behavior for the Engine renderer system.

#include "engine/renderer/texture_loader.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/string_util.h"
#include "engine/core/vfs.h"
#include "engine/math/vec3.h"
#include "texture_handle_codec.h"
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

/// Maps an stb channel count onto the engine 8-bit texel format.
TextureFormat ldr_format_for_channels(int channels) noexcept {
  switch (channels) {
  case 1:
    return TextureFormat::R8;
  case 2:
    return TextureFormat::RG8;
  case 3:
    return TextureFormat::RGB8;
  default:
    return TextureFormat::RGBA8;
  }
}

/// Maps an stb channel count onto the engine half-float texel format.
TextureFormat hdr_format_for_channels(int channels) noexcept {
  switch (channels) {
  case 1:
    return TextureFormat::R16F;
  case 2:
    return TextureFormat::RG16F;
  case 3:
    return TextureFormat::RGB16F;
  default:
    return TextureFormat::RGBA16F;
  }
}

struct TextureSlot final {
  DeviceTextureHandle device{};
  std::uint32_t generation = 1U;
  bool occupied = false;
  bool hdr = false;
  bool cubemap = false;
  // External slots alias a device texture owned elsewhere (e.g. a scene
  // capture target); the texture system never destroys their object.
  bool external = false;
  std::array<char, kMaxPathLen> path{};
};

struct TextureSystemState final {
  bool initialized = false;
  std::array<TextureSlot, kMaxTextureSlots> slots{};
};

TextureSystemState g_texState{};


/// Builds an externally visible handle for a live texture slot.
TextureHandle make_texture_handle(std::size_t slotIndex) noexcept {
  if ((slotIndex == 0U) || (slotIndex >= kMaxTextureSlots)) {
    return kInvalidTextureHandle;
  }

  const TextureSlot &slot = g_texState.slots[slotIndex];
  return texture_handle_detail::make_handle(slotIndex, slot.generation);
}

/// Resets a texture slot while preserving stale-handle invalidation.
void reset_texture_slot(TextureSlot &slot) noexcept {
  slot = TextureSlot{kInvalidDeviceTexture,
                     texture_handle_detail::next_generation(slot.generation),
                     false, false, false, false, {}};
}

/// Copies a texture VFS path into a fixed-size slot field.
void safe_copy_path(char *dst, std::size_t dstSize, const char *src) noexcept {
  core::copy_string(dst, dstSize, src);
}

/// Finds a free texture slot; slot 0 is reserved as the invalid handle.
std::size_t find_free_texture_slot() noexcept {
  for (std::size_t i = 1U; i < kMaxTextureSlots; ++i) {
    if (!g_texState.slots[i].occupied) {
      return i;
    }
  }

  return 0U;
}

TextureSlot *lookup_texture_slot(TextureHandle handle,
                                 std::size_t *outSlotIndex = nullptr) noexcept {
  if (!g_texState.initialized || handle == kInvalidTextureHandle) {
    return nullptr;
  }

  const std::uint32_t slotIndex = texture_handle_detail::slot_index(handle);
  const std::uint32_t generation = texture_handle_detail::generation(handle);
  if ((slotIndex == 0U) || (slotIndex >= kMaxTextureSlots) ||
      (generation == 0U) || !g_texState.slots[slotIndex].occupied ||
      (g_texState.slots[slotIndex].generation != generation)) {
    return nullptr;
  }

  if (outSlotIndex != nullptr) {
    *outSlotIndex = static_cast<std::size_t>(slotIndex);
  }
  return &g_texState.slots[slotIndex];
}

} // namespace

bool texture_input_size_fits_stb(std::size_t fileSize,
                                 int *outStbSize) noexcept {
  if ((outStbSize == nullptr) ||
      (fileSize > static_cast<std::size_t>(std::numeric_limits<int>::max()))) {
    return false;
  }

  *outStbSize = static_cast<int>(fileSize);
  return true;
}

namespace {

// Decoded-image budgets (audit #210), enforced from the encoded header
// before any full decode — file size is no proxy for decoded size, since
// a compressible or hostile header can expand far beyond its bytes on
// disk. 16384 matches the common GL max texture size; 512 MiB bounds one
// decoded image (a 16k x 8k RGBA8, or an 8k x 4k RGBA32F HDR); the
// equirect conversion's transient total is bounded by construction at
// this cap plus the kMaxCubemapFaceSize six-face footprint (~1.5 GiB).
constexpr int kMaxDecodedTextureDimension = 16384;
constexpr std::uint64_t kMaxDecodedTextureBytes = 512ULL << 20U;

/// Logs one decode-budget rejection with the offending numbers.
void log_decode_budget_rejection(const char *label, const char *reason,
                                 long long a, long long b) noexcept {
  char message[640] = {};
  std::snprintf(message, sizeof(message),
                "texture exceeds the decode budget (%s: %lld vs %lld): %s",
                reason, a, b, (label != nullptr) ? label : "(null)");
  core::log_message(core::LogLevel::Error, "renderer", message);
}

} // namespace

bool texture_decode_within_budget(const unsigned char *encodedBytes,
                                  int encodedByteCount, bool decodeAsHdr,
                                  int forcedChannels, const char *label,
                                  std::uint64_t *outDecodedBytes) noexcept {
  if (outDecodedBytes != nullptr) {
    *outDecodedBytes = 0ULL;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  if ((encodedBytes == nullptr) || (encodedByteCount <= 0) ||
      (stbi_info_from_memory(encodedBytes, encodedByteCount, &width, &height,
                             &channels) == 0) ||
      (width <= 0) || (height <= 0) || (channels <= 0)) {
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "texture exceeds the decode budget (unreadable header): %s",
                  (label != nullptr) ? label : "(null)");
    core::log_message(core::LogLevel::Error, "renderer", message);
    return false;
  }

  if ((width > kMaxDecodedTextureDimension) ||
      (height > kMaxDecodedTextureDimension)) {
    log_decode_budget_rejection(
        label, "dimension", (width > height) ? width : height,
        kMaxDecodedTextureDimension);
    return false;
  }

  // Dimensions are capped above, so the 64-bit products cannot overflow.
  const int decodedChannels = (forcedChannels > 0) ? forcedChannels : channels;
  const std::uint64_t texels =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  const std::uint64_t bytesPerTexel =
      static_cast<std::uint64_t>(decodedChannels) *
      (decodeAsHdr ? sizeof(float) : sizeof(unsigned char));
  const std::uint64_t decodedBytes = texels * bytesPerTexel;
  if (decodedBytes > kMaxDecodedTextureBytes) {
    log_decode_budget_rejection(label, "decoded bytes",
                                static_cast<long long>(decodedBytes),
                                static_cast<long long>(kMaxDecodedTextureBytes));
    return false;
  }

  if (outDecodedBytes != nullptr) {
    *outDecodedBytes = decodedBytes;
  }
  return true;
}

namespace {

int clamp_int(int value, int minValue, int maxValue) noexcept {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

float clamp_float(float value, float minValue, float maxValue) noexcept {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

int wrap_index(int value, int count) noexcept {
  int result = value % count;
  if (result < 0) {
    result += count;
  }
  return result;
}

using math::Vec3;

/// Unit direction through a cubemap texel for equirect resampling.
Vec3 cube_face_direction(int face, float u, float v) noexcept {
  switch (face) {
  case 0:
    return math::normalize(Vec3(1.0F, -v, -u));
  case 1:
    return math::normalize(Vec3(-1.0F, -v, u));
  case 2:
    return math::normalize(Vec3(u, 1.0F, v));
  case 3:
    return math::normalize(Vec3(u, -1.0F, -v));
  case 4:
    return math::normalize(Vec3(u, -v, 1.0F));
  default:
    return math::normalize(Vec3(-u, -v, -1.0F));
  }
}

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
    if (g_texState.slots[i].occupied &&
        (g_texState.slots[i].device != kInvalidDeviceTexture) &&
        !g_texState.slots[i].external) {
      if ((dev != nullptr) && (dev->destroy_texture != nullptr)) {
        dev->destroy_texture(g_texState.slots[i].device);
      }
    }
    reset_texture_slot(g_texState.slots[i]);
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
  DeviceTextureHandle deviceTexture{};
  int stbFileSize = 0;
  if (!texture_input_size_fits_stb(fileSize, &stbFileSize)) {
    core::vfs_free(fileData);
    core::log_message(core::LogLevel::Error, "renderer",
                      "texture file is too large");
    return kInvalidTextureHandle;
  }

  // #210: reject over-budget decodes from the header, before stb
  // allocates the full decoded image.
  const bool decodeAsHdr = stbi_is_hdr_from_memory(fileBytes, stbFileSize) != 0;
  if (!texture_decode_within_budget(fileBytes, stbFileSize, decodeAsHdr, 0,
                                    virtualPath, nullptr)) {
    core::vfs_free(fileData);
    return kInvalidTextureHandle;
  }

  if (decodeAsHdr) {
      float *pixels = stbi_loadf_from_memory(
        fileBytes, stbFileSize, &width, &height, &channels, 0);
    core::vfs_free(fileData);

    if (pixels == nullptr) {
      core::log_message(core::LogLevel::Error, "renderer",
                        "failed to decode HDR texture");
      return kInvalidTextureHandle;
    }

    const RenderDevice *dev = render_device();
    if (dev != nullptr && dev->create_texture != nullptr) {
      TextureDesc desc{};
      desc.kind = TextureKind::Tex2D;
      desc.format = hdr_format_for_channels(channels);
      desc.width = width;
      desc.height = height;
      desc.mipLevels = 1;
      desc.filter = TextureFilter::Linear;
      desc.wrap = TextureWrap::Repeat;
      desc.pixelData = TexelData::F32;
      desc.pixels = pixels;
      deviceTexture = dev->create_texture(desc);
    }
    stbi_image_free(pixels);
    isHdr = true;
  } else {
      unsigned char *pixels = stbi_load_from_memory(
        fileBytes, stbFileSize, &width, &height, &channels, 0);
    core::vfs_free(fileData);

    if (pixels == nullptr) {
      core::log_message(core::LogLevel::Error, "renderer",
                        "failed to decode texture");
      return kInvalidTextureHandle;
    }

    const RenderDevice *dev = render_device();
    if (dev != nullptr && dev->create_texture != nullptr) {
      TextureDesc desc{};
      desc.kind = TextureKind::Tex2D;
      desc.format = ldr_format_for_channels(channels);
      desc.width = width;
      desc.height = height;
      desc.mipLevels = 0; // full generated chain, as image assets always had
      desc.filter = TextureFilter::LinearMipmap;
      desc.wrap = TextureWrap::Repeat;
      desc.pixelData = TexelData::U8;
      desc.pixels = pixels;
      deviceTexture = dev->create_texture(desc);
    }
    stbi_image_free(pixels);
  }

  if (deviceTexture == kInvalidDeviceTexture) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to create GPU texture");
    return kInvalidTextureHandle;
  }

  TextureSlot &slot = g_texState.slots[freeSlot];
  slot.device = deviceTexture;
  slot.occupied = true;
  slot.hdr = isHdr;
  slot.cubemap = false;
  safe_copy_path(slot.path.data(), slot.path.size(), virtualPath);

  return make_texture_handle(freeSlot);
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
  int stbFileSize = 0;
  if (!texture_input_size_fits_stb(fileSize, &stbFileSize)) {
    core::vfs_free(fileData);
    core::log_message(core::LogLevel::Error, "renderer",
                      "HDR equirect texture file is too large");
    return kInvalidTextureHandle;
  }

  if (stbi_is_hdr_from_memory(fileBytes, stbFileSize) == 0) {
    core::vfs_free(fileData);
    core::log_message(core::LogLevel::Error, "renderer",
                      "equirect cubemap import requires an HDR texture");
    return kInvalidTextureHandle;
  }

  // #210: the conversion holds the decoded source and six faces at once.
  // The source decode is budgeted here from the header; faceSize is
  // already capped at kMaxCubemapFaceSize above, so the transient total is
  // bounded by construction at kMaxDecodedTextureBytes source + ~1.5 GiB
  // faces — no separate total check can trip while both caps hold.
  if (!texture_decode_within_budget(fileBytes, stbFileSize, true,
                                    kHdrCubemapChannels, virtualPath,
                                    nullptr)) {
    core::vfs_free(fileData);
    return kInvalidTextureHandle;
  }

  int width = 0;
  int height = 0;
  int sourceChannels = 0;
  float *pixels =
      stbi_loadf_from_memory(fileBytes, stbFileSize, &width, &height,
                             &sourceChannels, kHdrCubemapChannels);
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
  if ((dev == nullptr) || (dev->create_texture == nullptr)) {
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

  std::array<const void *, 6> facePixels{};
  for (std::size_t i = 0U; i < facePixels.size(); ++i) {
    facePixels[i] = faces[i].get();
  }

  TextureDesc cubeDesc{};
  cubeDesc.kind = TextureKind::Cube;
  cubeDesc.format = TextureFormat::RGB16F;
  cubeDesc.width = faceSize;
  cubeDesc.mipLevels = 0; // generated chain (prefilter source sampling)
  cubeDesc.filter = TextureFilter::LinearMipmap;
  cubeDesc.wrap = TextureWrap::ClampEdge;
  cubeDesc.pixelData = TexelData::F32;
  cubeDesc.facePixels = facePixels.data();
  const DeviceTextureHandle deviceTexture = dev->create_texture(cubeDesc);
  if (deviceTexture == kInvalidDeviceTexture) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to upload HDR cubemap texture");
    return kInvalidTextureHandle;
  }

  TextureSlot &slot = g_texState.slots[freeSlot];
  slot.device = deviceTexture;
  slot.occupied = true;
  slot.hdr = true;
  slot.cubemap = true;
  safe_copy_path(slot.path.data(), slot.path.size(), virtualPath);

  return make_texture_handle(freeSlot);
}

void unload_texture(TextureHandle handle) noexcept {
  if (!g_texState.initialized || handle == kInvalidTextureHandle) {
    return;
  }

  std::size_t slotIndex = 0U;
  TextureSlot *slot = lookup_texture_slot(handle, &slotIndex);
  if (slot == nullptr) {
    return;
  }

  const RenderDevice *dev = render_device();
  if ((dev != nullptr) && (dev->destroy_texture != nullptr) &&
      (slot->device != kInvalidDeviceTexture) && !slot->external) {
    dev->destroy_texture(slot->device);
  }

  reset_texture_slot(g_texState.slots[slotIndex]);
}

TextureHandle register_external_texture(
    DeviceTextureHandle texture) noexcept {
  if (!g_texState.initialized) {
    return kInvalidTextureHandle;
  }

  const std::size_t freeSlot = find_free_texture_slot();
  if (freeSlot == 0U) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "texture registry full");
    return kInvalidTextureHandle;
  }

  TextureSlot &slot = g_texState.slots[freeSlot];
  slot.occupied = true;
  slot.external = true;
  slot.device = texture;
  slot.hdr = false;
  slot.cubemap = false;
  safe_copy_path(slot.path.data(), slot.path.size(), "<external>");
  return make_texture_handle(freeSlot);
}

bool update_external_texture(TextureHandle handle,
                             DeviceTextureHandle texture) noexcept {
  TextureSlot *slot = lookup_texture_slot(handle);
  if ((slot == nullptr) || !slot->external) {
    return false;
  }

  slot->device = texture;
  return true;
}

DeviceTextureHandle texture_device_handle(TextureHandle handle) noexcept {
  const TextureSlot *slot = lookup_texture_slot(handle);
  if (slot == nullptr) {
    return kInvalidDeviceTexture;
  }

  return slot->device;
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
