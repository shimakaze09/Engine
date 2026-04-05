#include "engine/renderer/mesh_loader.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/mesh_asset.h"
#include "engine/core/platform.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

namespace {

constexpr std::size_t kVertexStrideFloats = 6U;
constexpr std::uint32_t kMaxMeshVertexCount = 1000000U;
constexpr std::uint32_t kMaxMeshIndexCount = 3000000U;

bool read_exact(FILE *file, void *data, std::size_t sizeBytes) noexcept {
  if ((file == nullptr) || (data == nullptr) || (sizeBytes == 0U)) {
    return false;
  }

  return std::fread(data, 1U, sizeBytes, file) == sizeBytes;
}

bool checked_mul(std::size_t lhs,
                 std::size_t rhs,
                 std::size_t *outResult) noexcept {
  if (outResult == nullptr) {
    return false;
  }

  if ((lhs != 0U) && (rhs > (std::numeric_limits<std::size_t>::max() / lhs))) {
    return false;
  }

  *outResult = lhs * rhs;
  return true;
}

bool checked_add(std::size_t lhs,
                 std::size_t rhs,
                 std::size_t *outResult) noexcept {
  if (outResult == nullptr) {
    return false;
  }

  if (lhs > (std::numeric_limits<std::size_t>::max() - rhs)) {
    return false;
  }

  *outResult = lhs + rhs;
  return true;
}

struct MeshBlob final {
  core::MeshAssetHeader header{};
  std::unique_ptr<float[]> vertices{};
  std::unique_ptr<std::uint32_t[]> indices{};
  std::size_t vertexFloatCount = 0U;
  std::size_t indexCount = 0U;
};

bool load_mesh_blob(const char *path, MeshBlob *outBlob) noexcept {
  if ((path == nullptr) || (outBlob == nullptr)) {
    return false;
  }

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

  core::MeshAssetHeader header{};
  if (!read_exact(file, &header, sizeof(header))) {
    std::fclose(file);
    return false;
  }

  if ((header.magic != core::kMeshAssetMagic)
      || (header.version != core::kMeshAssetVersion)) {
    std::fclose(file);
    return false;
  }

  if ((header.vertexCount == 0U) || (header.vertexCount > kMaxMeshVertexCount)
      || (header.indexCount > kMaxMeshIndexCount)) {
    std::fclose(file);
    return false;
  }

  std::size_t vertexFloatCount = 0U;
  if (!checked_mul(static_cast<std::size_t>(header.vertexCount),
                   kVertexStrideFloats,
                   &vertexFloatCount)) {
    std::fclose(file);
    return false;
  }

  std::size_t vertexBytes = 0U;
  if (!checked_mul(vertexFloatCount, sizeof(float), &vertexBytes)) {
    std::fclose(file);
    return false;
  }

  std::size_t indexBytes = 0U;
  if (!checked_mul(static_cast<std::size_t>(header.indexCount),
                   sizeof(std::uint32_t),
                   &indexBytes)) {
    std::fclose(file);
    return false;
  }

  std::size_t expectedSize = 0U;
  if (!checked_add(sizeof(core::MeshAssetHeader), vertexBytes, &expectedSize)
      || !checked_add(expectedSize, indexBytes, &expectedSize)) {
    std::fclose(file);
    return false;
  }

  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }

  const long fileSizeLong = std::ftell(file);
  if (fileSizeLong < 0L) {
    std::fclose(file);
    return false;
  }

  const std::size_t fileSize = static_cast<std::size_t>(fileSizeLong);
  if ((fileSize != expectedSize)
      || (std::fseek(file, sizeof(header), SEEK_SET) != 0)) {
    std::fclose(file);
    return false;
  }

  std::unique_ptr<float[]> vertices{};
  if (vertexFloatCount > 0U) {
    vertices.reset(new (std::nothrow) float[vertexFloatCount]);
    if (vertices == nullptr) {
      std::fclose(file);
      return false;
    }

    if (!read_exact(file, vertices.get(), vertexBytes)) {
      std::fclose(file);
      return false;
    }
  }

  std::unique_ptr<std::uint32_t[]> indices{};
  const std::size_t indexCount = static_cast<std::size_t>(header.indexCount);
  if (indexCount > 0U) {
    indices.reset(new (std::nothrow) std::uint32_t[indexCount]);
    if (indices == nullptr) {
      std::fclose(file);
      return false;
    }

    if (!read_exact(file, indices.get(), indexBytes)) {
      std::fclose(file);
      return false;
    }
  }

  std::fclose(file);
  outBlob->header = header;
  outBlob->vertexFloatCount = vertexFloatCount;
  outBlob->indexCount = indexCount;
  outBlob->vertices = std::move(vertices);
  outBlob->indices = std::move(indices);
  return true;
}

void delete_mesh_resources(const RenderDevice *dev, GpuMesh *mesh) noexcept {
  if ((mesh == nullptr) || (dev == nullptr)) {
    return;
  }

  if (mesh->indexBuffer != 0U) {
    dev->destroy_buffer(mesh->indexBuffer);
    mesh->indexBuffer = 0U;
  }

  if (mesh->vertexBuffer != 0U) {
    dev->destroy_buffer(mesh->vertexBuffer);
    mesh->vertexBuffer = 0U;
  }

  if (mesh->vertexArray != 0U) {
    dev->destroy_vertex_array(mesh->vertexArray);
    mesh->vertexArray = 0U;
  }

  mesh->vertexCount = 0U;
  mesh->indexCount = 0U;
}

} // namespace

bool load_mesh_from_file(const char *path, GpuMesh *outMesh) noexcept {
  if ((path == nullptr) || (outMesh == nullptr)) {
    return false;
  }

  *outMesh = GpuMesh{};

  MeshBlob meshBlob{};
  if (!load_mesh_blob(path, &meshBlob)) {
    core::log_message(
        core::LogLevel::Error, "renderer", "failed to read mesh asset");
    return false;
  }

  if (!core::make_render_context_current()) {
    core::log_message(core::LogLevel::Error,
                      "renderer",
                      "failed to make render context current for mesh upload");
    return false;
  }

  if (!initialize_render_device()) {
    core::release_render_context();
    return false;
  }

  const RenderDevice *dev = render_device();

  GpuMesh mesh{};
  mesh.vertexArray = dev->create_vertex_array();
  dev->bind_vertex_array(mesh.vertexArray);

  mesh.vertexBuffer = dev->create_buffer();
  dev->bind_array_buffer(mesh.vertexBuffer);
  dev->buffer_data_array(
      meshBlob.vertices.get(),
      static_cast<std::ptrdiff_t>(meshBlob.vertexFloatCount * sizeof(float)));

  dev->enable_vertex_attrib(0U);
  dev->vertex_attrib_float(
      0U,
      3,
      static_cast<std::int32_t>(kVertexStrideFloats * sizeof(float)),
      nullptr);

  dev->enable_vertex_attrib(1U);
  dev->vertex_attrib_float(
      1U,
      3,
      static_cast<std::int32_t>(kVertexStrideFloats * sizeof(float)),
      reinterpret_cast<const void *>(sizeof(float) * 3U));

  if (meshBlob.indexCount > 0U) {
    mesh.indexBuffer = dev->create_buffer();
    dev->bind_element_buffer(mesh.indexBuffer);
    dev->buffer_data_element(meshBlob.indices.get(),
                             static_cast<std::ptrdiff_t>(
                                 meshBlob.indexCount * sizeof(std::uint32_t)));
  }

  dev->bind_vertex_array(0U);
  dev->bind_array_buffer(0U);
  dev->bind_element_buffer(0U);

  mesh.vertexCount = meshBlob.header.vertexCount;
  mesh.indexCount = meshBlob.header.indexCount;
  *outMesh = mesh;

  core::release_render_context();
  return true;
}

void unload_mesh(GpuMesh *mesh) noexcept {
  if (mesh == nullptr) {
    return;
  }

  if ((mesh->vertexArray == 0U) && (mesh->vertexBuffer == 0U)
      && (mesh->indexBuffer == 0U)) {
    *mesh = GpuMesh{};
    return;
  }

  if (!core::make_render_context_current()) {
    core::log_message(core::LogLevel::Warning,
                      "renderer",
                      "failed to make context current for mesh unload");
    *mesh = GpuMesh{};
    return;
  }

  const RenderDevice *dev = render_device();
  if (dev != nullptr) {
    delete_mesh_resources(dev, mesh);
  } else {
    *mesh = GpuMesh{};
  }

  core::release_render_context();
}

std::uint32_t register_gpu_mesh(GpuMeshRegistry *registry,
                                const GpuMesh &mesh) noexcept {
  if (registry == nullptr) {
    return 0U;
  }

  for (std::size_t i = 1U; i < registry->meshes.size(); ++i) {
    if (!registry->occupied[i]) {
      registry->meshes[i] = mesh;
      registry->occupied[i] = true;
      return static_cast<std::uint32_t>(i);
    }
  }

  return 0U;
}

const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *registry,
                               renderer::MeshHandle handle) noexcept {
  if (registry == nullptr) {
    return nullptr;
  }

  const std::uint32_t id = handle.id;
  if ((id == 0U) || (id >= registry->meshes.size())) {
    return nullptr;
  }

  if (!registry->occupied[id]) {
    return nullptr;
  }

  return &registry->meshes[id];
}

} // namespace engine::renderer