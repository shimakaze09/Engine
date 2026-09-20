// Cooked-asset fixtures shared by the tests that exercise the generation
// gate: a minimal valid cooked mesh, the packer's output-manifest hash,
// and a cook stamp written from OUTPUT lines.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/content/cook_contract.h"
#include "engine/core/hash.h"
#include "engine/core/mesh_asset.h"

namespace engine::tests {

/// Opens `path` for binary writing; false when the open fails.
inline bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

/// Writes raw bytes to a file.
inline bool write_bytes(const char *path, const void *data,
                        std::size_t size) noexcept {
  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }
  const bool ok = (size == 0U) || (std::fwrite(data, 1U, size, file) == size);
  return (std::fclose(file) == 0) && ok;
}

/// FNV-1a of a file's bytes, matching the packer's output-manifest hash.
inline bool hash_file(const char *path, std::uint64_t *outHash) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }
  std::uint64_t hash = engine::core::kFnv1a64Offset;
  unsigned char buffer[512] = {};
  std::size_t bytesRead = 0U;
  while ((bytesRead = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    for (std::size_t i = 0U; i < bytesRead; ++i) {
      hash = engine::core::fnv1a_64_append(hash, buffer[i]);
    }
  }
  std::fclose(file);
  *outHash = hash;
  return true;
}

/// Writes a minimal valid cooked mesh (1 position/normal vertex).
inline bool write_valid_mesh(const char *path) noexcept {
  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 6U> vertexData = {0.0F, 0.0F, 0.0F,
                                            0.0F, 1.0F, 0.0F};
  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }
  bool ok = std::fwrite(&header, sizeof(header), 1U, file) == 1U;
  ok = ok && (std::fwrite(vertexData.data(), sizeof(float), vertexData.size(),
                          file) == vertexData.size());
  return (std::fclose(file) == 0) && ok;
}

/// Writes a current-schema stamp for `meshPath` from OUTPUT lines.
inline bool write_stamp(const char *meshPath,
                        const char *outputLines) noexcept {
  char stampPath[512] = {};
  std::snprintf(stampPath, sizeof(stampPath), "%s.cookstamp", meshPath);
  char text[2048] = {};
  const int written = std::snprintf(
      text, sizeof(text),
      "SCHEMA %u\nTOOL_VERSION %u\nSOURCE_HASH 0000000000000001\n"
      "IMPORT_HASH 0000000000000002\nPLATFORM TestPlat\n%s",
      static_cast<unsigned int>(engine::content::kCookStampSchema),
      static_cast<unsigned int>(engine::content::kCookToolVersion),
      outputLines);
  if ((written <= 0) || (written >= static_cast<int>(sizeof(text)))) {
    return false;
  }
  return write_bytes(stampPath, text, static_cast<std::size_t>(written));
}

/// Writes a stamp for `meshPath` from complete text.
inline bool write_stamp_text(const char *meshPath, const char *text) noexcept {
  char stampPath[512] = {};
  std::snprintf(stampPath, sizeof(stampPath), "%s.cookstamp", meshPath);
  return write_bytes(stampPath, text, std::strlen(text));
}

/// Writes a stamp whose manifest certifies `meshPath` with its current
/// bytes, the shape an intact cook leaves behind.
inline bool write_certifying_stamp(const char *meshPath) noexcept {
  std::uint64_t meshHash = 0ULL;
  if (!hash_file(meshPath, &meshHash)) {
    return false;
  }
  char outputs[512] = {};
  std::snprintf(outputs, sizeof(outputs), "OUTPUT %016llx %s\n",
                static_cast<unsigned long long>(meshHash), meshPath);
  return write_stamp(meshPath, outputs);
}

/// Removes the mesh and its stamp.
inline void remove_with_stamp(const char *meshPath) noexcept {
  char stampPath[512] = {};
  std::snprintf(stampPath, sizeof(stampPath), "%s.cookstamp", meshPath);
  static_cast<void>(std::remove(meshPath));
  static_cast<void>(std::remove(stampPath));
}

} // namespace engine::tests
