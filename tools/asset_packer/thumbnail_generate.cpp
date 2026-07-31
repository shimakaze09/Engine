// Implements packer thumbnail generation for textures and meshes, with
// content checksums so up-to-date thumbnails are skipped.

#include "packer_shared.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <stb_image.h>
#include <stb_image_write.h>

#include "thumbnail_resample.h"

// E.g. ".thumbnails/foo.png" -> ".thumbnails/foo.png.checksum".
static void build_checksum_path(const char *thumbPath, char *checksumPath,
                                std::size_t size) noexcept {
  std::strncpy(checksumPath, thumbPath, size - 1U);
  checksumPath[size - 1U] = '\0';
  char *dot = std::strrchr(checksumPath, '.');
  if (dot != nullptr) {
    std::strncpy(dot, ".checksum",
                 size - static_cast<std::size_t>(dot - checksumPath) - 1U);
    checksumPath[size - 1U] = '\0';
  } else {
    std::strncat(checksumPath, ".checksum",
                 size - std::strlen(checksumPath) - 1U);
  }
}

// Read stored checksum from sidecar file. Returns false if file not found.
static bool read_thumbnail_checksum(const char *checksumPath,
                                    std::uint64_t *outHash) noexcept {
  if ((checksumPath == nullptr) || (outHash == nullptr)) {
    return false;
  }

  FILE *f = std::fopen(checksumPath, "r");
  if (f == nullptr) {
    return false;
  }

  unsigned long long parsedHash = 0ULL;
  const int scanned = std::fscanf(f, "%llu", &parsedHash); // NOLINT
  std::fclose(f);
  if (scanned != 1) {
    return false;
  }

  *outHash = static_cast<std::uint64_t>(parsedHash);
  return true;
}

// Write checksum to sidecar file.
static bool write_thumbnail_checksum(const char *checksumPath,
                                     std::uint64_t hash) noexcept {
  FILE *f = std::fopen(checksumPath, "w");
  if (f == nullptr) {
    return false;
  }
  std::fprintf(f, "%llu", static_cast<unsigned long long>(hash)); // NOLINT
  std::fclose(f);
  return true;
}

// Forward declaration (defined below alongside mesh thumbnail).
void build_thumbnail_path(const char *outputPath, char *thumbPath,
                          std::size_t thumbPathSize) noexcept;

// ---------------------------------------------------------------------------
// Texture thumbnail generation
// ---------------------------------------------------------------------------

bool generate_texture_thumbnail(const char *inputPath,
                                const char *outputPath) noexcept {
  if ((inputPath == nullptr) || (outputPath == nullptr)) {
    return false;
  }

  char thumbPath[512] = {};
  build_thumbnail_path(outputPath, thumbPath, sizeof(thumbPath));

  bool hashOk = false;
  const std::uint64_t srcHash = hash_file_contents(inputPath, &hashOk);
  if (hashOk) {
    char checksumPath[512] = {};
    build_checksum_path(thumbPath, checksumPath, sizeof(checksumPath));
    std::uint64_t storedHash = 0U;
    if (read_thumbnail_checksum(checksumPath, &storedHash) &&
        storedHash == srcHash) {
      std::printf("thumbnail up-to-date; skipped: %s\n", thumbPath);
      return true;
    }
  }

  int srcW = 0, srcH = 0, srcChannels = 0;
  stbi_uc *srcPixels = stbi_load(inputPath, &srcW, &srcH, &srcChannels, 4);
  if (srcPixels == nullptr) {
    std::fprintf(stderr, "thumbnail: failed to load %s\n", inputPath);
    return false;
  }

  constexpr int kThumbSize = 64;
  constexpr int kChannels = 4;

  // Progressive halving (mip-chain style) beats one-shot box filtering.
  std::vector<std::uint8_t> current(static_cast<std::size_t>(srcW) *
                                    static_cast<std::size_t>(srcH) *
                                    static_cast<std::size_t>(kChannels));
  std::memcpy(current.data(), srcPixels, current.size());
  stbi_image_free(srcPixels);

  int curW = srcW, curH = srcH;

  while ((curW > kThumbSize) || (curH > kThumbSize)) {
    const int newW = std::max(curW / 2, 1);
    const int newH = std::max(curH / 2, 1);
    std::vector<std::uint8_t> next(static_cast<std::size_t>(newW) *
                                       static_cast<std::size_t>(newH) *
                                       static_cast<std::size_t>(kChannels),
                                   0U);

    for (int y = 0; y < newH; ++y) {
      for (int x = 0; x < newW; ++x) {
        const int sx = x * 2;
        const int sy = y * 2;
        for (int c = 0; c < kChannels; ++c) {
          std::uint32_t sum = 0U;
          std::uint32_t count = 0U;
          for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
              const int px = std::min(sx + dx, curW - 1);
              const int py = std::min(sy + dy, curH - 1);
              const std::size_t sourceIndex =
                  (static_cast<std::size_t>(py) *
                       static_cast<std::size_t>(curW) +
                   static_cast<std::size_t>(px)) *
                      static_cast<std::size_t>(kChannels) +
                  static_cast<std::size_t>(c);
              sum += static_cast<std::uint32_t>(current[sourceIndex]);
              ++count;
            }
          }
          const std::size_t destinationIndex =
              (static_cast<std::size_t>(y) * static_cast<std::size_t>(newW) +
               static_cast<std::size_t>(x)) *
                  static_cast<std::size_t>(kChannels) +
              static_cast<std::size_t>(c);
          next[destinationIndex] = static_cast<std::uint8_t>(sum / count);
        }
      }
    }

    current = std::move(next);
    curW = newW;
    curH = newH;
  }

  std::vector<std::uint8_t> thumb(
      static_cast<std::size_t>(kThumbSize * kThumbSize * kChannels), 0U);
  if (!engine::tools::resize_rgba_bilinear(
          current.data(), curW, curH, thumb.data(), kThumbSize, kThumbSize)) {
    std::fprintf(stderr, "thumbnail: invalid image dimensions for %s\n",
                 inputPath);
    return false;
  }

  if (!stbi_write_png(thumbPath, kThumbSize, kThumbSize, kChannels,
                      thumb.data(), kThumbSize * kChannels)) {
    std::fprintf(stderr, "thumbnail: failed to write %s\n", thumbPath);
    return false;
  }

  if (hashOk) {
    char checksumPath[512] = {};
    build_checksum_path(thumbPath, checksumPath, sizeof(checksumPath));
    write_thumbnail_checksum(checksumPath, srcHash);
  }

  std::printf("texture thumbnail: %s\n", thumbPath);
  return true;
}

// Build thumbnail path: <dir>/.thumbnails/<basename>.png
void build_thumbnail_path(const char *outputPath, char *thumbPath,
                          std::size_t thumbPathSize) noexcept {
  const char *lastSlash = std::strrchr(outputPath, '/');
  const char *lastBackSlash = std::strrchr(outputPath, '\\');
  if ((lastBackSlash != nullptr) &&
      ((lastSlash == nullptr) || (lastBackSlash > lastSlash))) {
    lastSlash = lastBackSlash;
  }

  char thumbDir[512] = {};
  if (lastSlash != nullptr) {
    const std::size_t dirLen = static_cast<std::size_t>(lastSlash - outputPath);
    if (dirLen < sizeof(thumbDir) - 13U) {
      std::memcpy(thumbDir, outputPath, dirLen);
      std::snprintf(thumbDir + dirLen, sizeof(thumbDir) - dirLen,
                    "/.thumbnails");
    } else {
      std::snprintf(thumbDir, sizeof(thumbDir), ".thumbnails");
    }
  } else {
    std::snprintf(thumbDir, sizeof(thumbDir), ".thumbnails");
  }
  ensure_directory_exists(thumbDir);

  const char *basename = (lastSlash != nullptr) ? (lastSlash + 1) : outputPath;
  std::snprintf(thumbPath, thumbPathSize, "%s/%s.png", thumbDir, basename);
}

bool generate_mesh_thumbnail(const char *inputPath, const char *outputPath,
                             const PrimitiveData &data) {
  if (outputPath == nullptr) {
    return false;
  }

  char thumbPath[512] = {};
  build_thumbnail_path(outputPath, thumbPath, sizeof(thumbPath));

  if (inputPath != nullptr) {
    bool hashOk = false;
    const std::uint64_t srcHash = hash_file_contents(inputPath, &hashOk);
    if (hashOk) {
      char checksumPath[512] = {};
      build_checksum_path(thumbPath, checksumPath, sizeof(checksumPath));
      std::uint64_t storedHash = 0U;
      if (read_thumbnail_checksum(checksumPath, &storedHash) &&
          storedHash == srcHash) {
        std::printf("thumbnail up-to-date; skipped: %s\n", thumbPath);
        return true;
      }
    }
  }

  constexpr int kThumbSize = 64;
  constexpr int kChannels = 4;
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(kThumbSize * kThumbSize * kChannels), 0U);
  std::vector<float> depth(static_cast<std::size_t>(kThumbSize * kThumbSize),
                           1e30F);

  const std::size_t strideFloats = primitive_stride_floats(data);
  const std::size_t vertexCount =
      data.interleavedVertices.size() / strideFloats;
  if (vertexCount == 0U) {
    return false;
  }

  float minX = 1e30F;
  float minY = 1e30F;
  float minZ = 1e30F;
  float maxX = -1e30F;
  float maxY = -1e30F;
  float maxZ = -1e30F;
  for (std::size_t i = 0U; i < vertexCount; ++i) {
    const std::size_t base = i * strideFloats;
    const float x = data.interleavedVertices[base + 0U];
    const float y = data.interleavedVertices[base + 1U];
    const float z = data.interleavedVertices[base + 2U];
    if (x < minX) {
      minX = x;
    }
    if (y < minY) {
      minY = y;
    }
    if (z < minZ) {
      minZ = z;
    }
    if (x > maxX) {
      maxX = x;
    }
    if (y > maxY) {
      maxY = y;
    }
    if (z > maxZ) {
      maxZ = z;
    }
  }

  const float cx = (minX + maxX) * 0.5F;
  const float cy = (minY + maxY) * 0.5F;
  const float dx = maxX - minX;
  const float dy = maxY - minY;
  const float dz = maxZ - minZ;
  float extent = dx;
  if (dy > extent) {
    extent = dy;
  }
  if (dz > extent) {
    extent = dz;
  }
  if (extent < 1e-6F) {
    extent = 1.0F;
  }
  const float invExtent = static_cast<float>(kThumbSize - 4) / extent;

  // Simple orthographic projection from +Z looking at center.
  // Light direction: normalized (0.5, 0.7, 1.0).
  constexpr float kLightX = 0.365148F;
  constexpr float kLightY = 0.511208F;
  constexpr float kLightZ = 0.730297F;

  auto project = [&](std::size_t vi, float *sx, float *sy, float *sz) {
    const std::size_t base = vi * strideFloats;
    const float x = data.interleavedVertices[base + 0U];
    const float y = data.interleavedVertices[base + 1U];
    const float z = data.interleavedVertices[base + 2U];
    *sx = (x - cx) * invExtent + static_cast<float>(kThumbSize) * 0.5F;
    *sy = static_cast<float>(kThumbSize) * 0.5F - (y - cy) * invExtent;
    *sz = z;
  };

  auto rasterizeTriangle = [&](std::size_t i0, std::size_t i1, std::size_t i2) {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float z0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    float z1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    float z2 = 0.0F;
    project(i0, &x0, &y0, &z0);
    project(i1, &x1, &y1, &z1);
    project(i2, &x2, &y2, &z2);

    const std::size_t b0 = i0 * strideFloats;
    const std::size_t b1 = i1 * strideFloats;
    const std::size_t b2 = i2 * strideFloats;
    const float nx =
        (data.interleavedVertices[b0 + 3U] + data.interleavedVertices[b1 + 3U] +
         data.interleavedVertices[b2 + 3U]) /
        3.0F;
    const float ny =
        (data.interleavedVertices[b0 + 4U] + data.interleavedVertices[b1 + 4U] +
         data.interleavedVertices[b2 + 4U]) /
        3.0F;
    const float nz =
        (data.interleavedVertices[b0 + 5U] + data.interleavedVertices[b1 + 5U] +
         data.interleavedVertices[b2 + 5U]) /
        3.0F;
    float dot = nx * kLightX + ny * kLightY + nz * kLightZ;
    if (dot < 0.0F) {
      dot = 0.0F;
    }
    const float ambient = 0.15F;
    const float shade = ambient + (1.0F - ambient) * dot;
    const auto color = static_cast<std::uint8_t>(
        shade > 1.0F ? 255U : static_cast<unsigned>(shade * 255.0F));

    float fminX = x0;
    if (x1 < fminX) {
      fminX = x1;
    }
    if (x2 < fminX) {
      fminX = x2;
    }
    float fmaxX = x0;
    if (x1 > fmaxX) {
      fmaxX = x1;
    }
    if (x2 > fmaxX) {
      fmaxX = x2;
    }
    float fminY = y0;
    if (y1 < fminY) {
      fminY = y1;
    }
    if (y2 < fminY) {
      fminY = y2;
    }
    float fmaxY = y0;
    if (y1 > fmaxY) {
      fmaxY = y1;
    }
    if (y2 > fmaxY) {
      fmaxY = y2;
    }

    const int ixMin = static_cast<int>(fminX);
    const int ixMax = static_cast<int>(fmaxX) + 1;
    const int iyMin = static_cast<int>(fminY);
    const int iyMax = static_cast<int>(fmaxY) + 1;

    for (int py = iyMin; py <= iyMax; ++py) {
      if ((py < 0) || (py >= kThumbSize)) {
        continue;
      }
      for (int px = ixMin; px <= ixMax; ++px) {
        if ((px < 0) || (px >= kThumbSize)) {
          continue;
        }
        const float pxf = static_cast<float>(px) + 0.5F;
        const float pyf = static_cast<float>(py) + 0.5F;
          const float d00 = (x1 - x0) * (pyf - y0) - (y1 - y0) * (pxf - x0);
        const float d01 = (x2 - x1) * (pyf - y1) - (y2 - y1) * (pxf - x1);
        const float d02 = (x0 - x2) * (pyf - y2) - (y0 - y2) * (pxf - x2);
        if ((d00 >= 0.0F && d01 >= 0.0F && d02 >= 0.0F) ||
            (d00 <= 0.0F && d01 <= 0.0F && d02 <= 0.0F)) {
              const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
          float z = z0;
          if (std::fabs(area) > 1e-8F) {
            const float invArea = 1.0F / area;
            const float w0 =
                ((x1 - pxf) * (y2 - pyf) - (y1 - pyf) * (x2 - pxf)) * invArea;
            const float w1 =
                ((x2 - pxf) * (y0 - pyf) - (y2 - pyf) * (x0 - pxf)) * invArea;
            const float w2 = 1.0F - w0 - w1;
            z = w0 * z0 + w1 * z1 + w2 * z2;
          }
          const std::size_t idx =
              static_cast<std::size_t>(py * kThumbSize + px);
          if (z < depth[idx]) {
            depth[idx] = z;
            const std::size_t pi = idx * kChannels;
            pixels[pi + 0U] = color;
            pixels[pi + 1U] = color;
            pixels[pi + 2U] = color;
            pixels[pi + 3U] = 255U;
          }
        }
      }
    }
  };

  if (!data.indices.empty()) {
    for (std::size_t i = 0U; i + 2U < data.indices.size(); i += 3U) {
      rasterizeTriangle(data.indices[i + 0U], data.indices[i + 1U],
                        data.indices[i + 2U]);
    }
  } else {
    for (std::size_t i = 0U; i + 2U < vertexCount; i += 3U) {
      rasterizeTriangle(i + 0U, i + 1U, i + 2U);
    }
  }

  for (std::size_t i = 0U;
       i < static_cast<std::size_t>(kThumbSize * kThumbSize); ++i) {
    if (pixels[i * kChannels + 3U] == 0U) {
      pixels[i * kChannels + 0U] = 48U;
      pixels[i * kChannels + 1U] = 48U;
      pixels[i * kChannels + 2U] = 48U;
      pixels[i * kChannels + 3U] = 255U;
    }
  }

  if (!stbi_write_png(thumbPath, kThumbSize, kThumbSize, kChannels,
                      pixels.data(), kThumbSize * kChannels)) {
    std::fprintf(stderr, "warning: failed to write thumbnail: %s\n", thumbPath);
    return false;
  }

  if (inputPath != nullptr) {
    bool hashOk = false;
    const std::uint64_t srcHash = hash_file_contents(inputPath, &hashOk);
    if (hashOk) {
      char checksumPath[512] = {};
      build_checksum_path(thumbPath, checksumPath, sizeof(checksumPath));
      write_thumbnail_checksum(checksumPath, srcHash);
    }
  }

  std::printf("generated thumbnail: %s\n", thumbPath);
  return true;
}
