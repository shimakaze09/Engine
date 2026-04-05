#include <array>
#include <cstddef>
#include <cstdio>

#include "engine/core/mesh_asset.h"
#include "engine/renderer/mesh_loader.h"

namespace {

constexpr const char *kBadMagicPath = "mesh_loader_bad_magic.mesh";
constexpr const char *kBadVersionPath = "mesh_loader_bad_version.mesh";
constexpr const char *kOversizedVertexPath =
    "mesh_loader_oversized_vertices.mesh";
constexpr const char *kOversizedIndexPath =
    "mesh_loader_oversized_indices.mesh";
constexpr const char *kFileSizeMismatchPath = "mesh_loader_size_mismatch.mesh";

void remove_file(const char *path) noexcept {
  if (path != nullptr) {
    static_cast<void>(std::remove(path));
  }
}

bool write_mesh_file(const char *path,
                     const engine::core::MeshAssetHeader &header,
                     const void *payload, std::size_t payloadBytes) noexcept {
  if (path == nullptr) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  bool ok = (std::fwrite(&header, sizeof(header), 1U, file) == 1U);
  if (ok && (payloadBytes > 0U)) {
    ok = (payload != nullptr) &&
         (std::fwrite(payload, 1U, payloadBytes, file) == payloadBytes);
  }

  std::fclose(file);
  return ok;
}

int check_bad_magic() {
  remove_file(kBadMagicPath);

  engine::core::MeshAssetHeader header{};
  header.magic = 0xDEADBEEFU;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 6U> vertexData = {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  if (!write_mesh_file(kBadMagicPath, header, vertexData.data(),
                       vertexData.size() * sizeof(float))) {
    remove_file(kBadMagicPath);
    return 11;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kBadMagicPath, &mesh);
  remove_file(kBadMagicPath);
  return loaded ? 12 : 0;
}

int check_bad_version() {
  remove_file(kBadVersionPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = 99U;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 6U> vertexData = {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  if (!write_mesh_file(kBadVersionPath, header, vertexData.data(),
                       vertexData.size() * sizeof(float))) {
    remove_file(kBadVersionPath);
    return 21;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kBadVersionPath, &mesh);
  remove_file(kBadVersionPath);
  return loaded ? 22 : 0;
}

int check_oversized_vertex_count() {
  remove_file(kOversizedVertexPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 2000000U;
  header.indexCount = 0U;

  if (!write_mesh_file(kOversizedVertexPath, header, nullptr, 0U)) {
    remove_file(kOversizedVertexPath);
    return 31;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kOversizedVertexPath, &mesh);
  remove_file(kOversizedVertexPath);
  return loaded ? 32 : 0;
}

int check_oversized_index_count() {
  remove_file(kOversizedIndexPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 4000000U;

  if (!write_mesh_file(kOversizedIndexPath, header, nullptr, 0U)) {
    remove_file(kOversizedIndexPath);
    return 41;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kOversizedIndexPath, &mesh);
  remove_file(kOversizedIndexPath);
  return loaded ? 42 : 0;
}

int check_file_size_mismatch() {
  remove_file(kFileSizeMismatchPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 100U;
  header.indexCount = 0U;

  const std::array<float, 10U> truncatedVertexData = {
      0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
  };
  if (!write_mesh_file(kFileSizeMismatchPath, header,
                       truncatedVertexData.data(),
                       truncatedVertexData.size() * sizeof(float))) {
    remove_file(kFileSizeMismatchPath);
    return 51;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kFileSizeMismatchPath, &mesh);
  remove_file(kFileSizeMismatchPath);
  return loaded ? 52 : 0;
}

int check_empty_path() {
  engine::renderer::GpuMesh mesh{};
  const bool loaded = engine::renderer::load_mesh_from_file(nullptr, &mesh);
  return loaded ? 61 : 0;
}

int check_null_out_param() {
  const bool loaded =
      engine::renderer::load_mesh_from_file("somepath", nullptr);
  return loaded ? 71 : 0;
}

} // namespace

int main() {
  int result = check_bad_magic();
  if (result != 0) {
    return result;
  }

  result = check_bad_version();
  if (result != 0) {
    return result;
  }

  result = check_oversized_vertex_count();
  if (result != 0) {
    return result;
  }

  result = check_oversized_index_count();
  if (result != 0) {
    return result;
  }

  result = check_file_size_mismatch();
  if (result != 0) {
    return result;
  }

  result = check_empty_path();
  if (result != 0) {
    return result;
  }

  result = check_null_out_param();
  if (result != 0) {
    return result;
  }

  return 0;
}
