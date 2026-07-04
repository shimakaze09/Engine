// Implements mesh loader behavior for the Engine renderer system.

#include "engine/renderer/mesh_loader.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/mesh_asset.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

namespace {

constexpr std::size_t kVertexStrideV1Floats = 6U;
constexpr std::size_t kVertexStrideV2Floats = 8U;
constexpr std::uint32_t kMaxMeshVertexCount = 1000000U;
constexpr std::uint32_t kMaxMeshIndexCount = 3000000U;

/// Reads exact data.
bool read_exact(FILE *file, void *data, std::size_t sizeBytes) noexcept {
  if ((file == nullptr) || (data == nullptr) || (sizeBytes == 0U)) {
    return false;
  }

  return std::fread(data, 1U, sizeBytes, file) == sizeBytes;
}

/// Handles checked mul.
bool checked_mul(std::size_t lhs, std::size_t rhs,
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

/// Handles checked add.
bool checked_add(std::size_t lhs, std::size_t rhs,
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

/// Handles delete mesh resources.
void delete_mesh_resources(const RenderDevice *dev, GpuMesh *mesh) noexcept {
  if ((mesh == nullptr) || (dev == nullptr)) {
    return;
  }

  if (mesh->indexBuffer != 0U) {
    if (dev->destroy_buffer != nullptr) {
      dev->destroy_buffer(mesh->indexBuffer);
    }
    mesh->indexBuffer = 0U;
  }

  if (mesh->vertexBuffer != 0U) {
    if (dev->destroy_buffer != nullptr) {
      dev->destroy_buffer(mesh->vertexBuffer);
    }
    mesh->vertexBuffer = 0U;
  }

  if (mesh->vertexArray != 0U) {
    if (dev->destroy_vertex_array != nullptr) {
      dev->destroy_vertex_array(mesh->vertexArray);
    }
    mesh->vertexArray = 0U;
  }

  mesh->vertexCount = 0U;
  mesh->indexCount = 0U;
}

bool mesh_upload_device_ready(const RenderDevice *dev) noexcept {
  return (dev != nullptr) && (dev->create_vertex_array != nullptr) &&
         (dev->destroy_vertex_array != nullptr) &&
         (dev->bind_vertex_array != nullptr) &&
         (dev->create_buffer != nullptr) && (dev->destroy_buffer != nullptr) &&
         (dev->bind_array_buffer != nullptr) &&
         (dev->bind_element_buffer != nullptr) &&
         (dev->buffer_data_array != nullptr) &&
         (dev->buffer_data_element != nullptr) &&
         (dev->enable_vertex_attrib != nullptr) &&
         (dev->vertex_attrib_float != nullptr);
}

void unbind_mesh_upload_state(const RenderDevice *dev) noexcept {
  if (dev == nullptr) {
    return;
  }
  if (dev->bind_vertex_array != nullptr) {
    dev->bind_vertex_array(0U);
  }
  if (dev->bind_array_buffer != nullptr) {
    dev->bind_array_buffer(0U);
  }
  if (dev->bind_element_buffer != nullptr) {
    dev->bind_element_buffer(0U);
  }
}

bool fail_mesh_upload(const RenderDevice *dev, GpuMesh *mesh,
                      GpuMesh *outMesh) noexcept {
  unbind_mesh_upload_state(dev);
  delete_mesh_resources(dev, mesh);
  if (outMesh != nullptr) {
    *outMesh = GpuMesh{};
  }
  core::log_message(core::LogLevel::Error, "renderer",
                    "failed to create mesh GPU resources");
  return false;
}

} // namespace

/// Decodes a cooked mesh file without touching GPU state.
bool load_mesh_data_from_file(const char *path, CpuMeshData *outData,
                              std::uint64_t *outSizeBytes) noexcept {
  if ((path == nullptr) || (outData == nullptr)) {
    return false;
  }

  *outData = CpuMeshData{};
  if (outSizeBytes != nullptr) {
    *outSizeBytes = 0ULL;
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

  if (header.magic != core::kMeshAssetMagic) {
    std::fclose(file);
    return false;
  }

  const bool isV1 = (header.version == core::kMeshAssetVersion);
  const bool isV2 = (header.version == core::kMeshAssetVersion2);
  if (!isV1 && !isV2) {
    std::fclose(file);
    return false;
  }

  const std::size_t strideFloats =
      isV2 ? kVertexStrideV2Floats : kVertexStrideV1Floats;

  if ((header.vertexCount == 0U) ||
      (header.vertexCount > kMaxMeshVertexCount) ||
      (header.indexCount > kMaxMeshIndexCount)) {
    std::fclose(file);
    return false;
  }

  std::size_t vertexFloatCount = 0U;
  if (!checked_mul(static_cast<std::size_t>(header.vertexCount), strideFloats,
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
                   sizeof(std::uint32_t), &indexBytes)) {
    std::fclose(file);
    return false;
  }

  std::size_t expectedSize = 0U;
  if (!checked_add(sizeof(core::MeshAssetHeader), vertexBytes, &expectedSize) ||
      !checked_add(expectedSize, indexBytes, &expectedSize)) {
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
  if ((fileSize != expectedSize) ||
      (std::fseek(file, sizeof(header), SEEK_SET) != 0)) {
    std::fclose(file);
    return false;
  }

  // Heap allocation here is intentional: mesh data is variable-size and may
  // exceed the frame allocator budget. This function performs CPU IO only.
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

  outData->vertexCount = header.vertexCount;
  outData->indexCount = header.indexCount;
  outData->vertexFloatCount = vertexFloatCount;
  outData->strideFloats = strideFloats;
  outData->hasUVs = isV2;
  outData->vertices = std::move(vertices);
  outData->indices = std::move(indices);
  if (outSizeBytes != nullptr) {
    *outSizeBytes = static_cast<std::uint64_t>(expectedSize);
  }
  return true;
}

// Precondition: caller must own the GL context before calling this function.
// Context acquisition and release are the engine loop's responsibility;
// the renderer must not acquire or release the context internally.
bool load_mesh_from_file(const char *path, GpuMesh *outMesh) noexcept {
  if ((path == nullptr) || (outMesh == nullptr)) {
    return false;
  }

  *outMesh = GpuMesh{};

  CpuMeshData meshData{};
  if (!load_mesh_data_from_file(path, &meshData)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to read mesh asset");
    return false;
  }

  return upload_mesh_data_to_gpu(meshData, outMesh);
}

// Precondition: caller must own the GL context before calling this function.
bool upload_mesh_data_to_gpu(const CpuMeshData &meshData,
                             GpuMesh *outMesh) noexcept {
  if ((outMesh == nullptr) || (meshData.vertexCount == 0U) ||
      (meshData.vertices == nullptr)) {
    return false;
  }

  *outMesh = GpuMesh{};

  if (!initialize_render_device()) {
    return false;
  }

  const RenderDevice *dev = render_device();
  if (!mesh_upload_device_ready(dev)) {
    return false;
  }
  const std::int32_t stride =
      static_cast<std::int32_t>(meshData.strideFloats * sizeof(float));

  GpuMesh mesh{};
  mesh.hasUVs = meshData.hasUVs;
  mesh.vertexArray = dev->create_vertex_array();
  if (mesh.vertexArray == 0U) {
    return fail_mesh_upload(dev, &mesh, outMesh);
  }
  dev->bind_vertex_array(mesh.vertexArray);

  mesh.vertexBuffer = dev->create_buffer();
  if (mesh.vertexBuffer == 0U) {
    return fail_mesh_upload(dev, &mesh, outMesh);
  }
  dev->bind_array_buffer(mesh.vertexBuffer);
  dev->buffer_data_array(
      meshData.vertices.get(),
      static_cast<std::ptrdiff_t>(meshData.vertexFloatCount * sizeof(float)));

  dev->enable_vertex_attrib(0U);
  dev->vertex_attrib_float(0U, 3, stride, nullptr);

  dev->enable_vertex_attrib(1U);
  dev->vertex_attrib_float(1U, 3, stride,
                           reinterpret_cast<const void *>(sizeof(float) * 3U));

  if (meshData.hasUVs) {
    dev->enable_vertex_attrib(2U);
    dev->vertex_attrib_float(
        2U, 2, stride, reinterpret_cast<const void *>(sizeof(float) * 6U));
  }

  if (meshData.indexCount > 0U) {
    if (meshData.indices == nullptr) {
      return fail_mesh_upload(dev, &mesh, outMesh);
    }
    mesh.indexBuffer = dev->create_buffer();
    if (mesh.indexBuffer == 0U) {
      return fail_mesh_upload(dev, &mesh, outMesh);
    }
    dev->bind_element_buffer(mesh.indexBuffer);
    dev->buffer_data_element(
        meshData.indices.get(),
        static_cast<std::ptrdiff_t>(static_cast<std::size_t>(
                                        meshData.indexCount) *
                                    sizeof(std::uint32_t)));
  }

  unbind_mesh_upload_state(dev);

  mesh.vertexCount = meshData.vertexCount;
  mesh.indexCount = meshData.indexCount;
  *outMesh = mesh;
  return true;
}

// Precondition: caller must own the GL context before calling this function.
bool build_gpu_mesh_from_data(const float *vertices, std::uint32_t vertexCount,
                              const std::uint32_t *indices,
                              std::uint32_t indexCount, bool hasUVs,
                              GpuMesh *outMesh) noexcept {
  if ((outMesh == nullptr) || (vertexCount == 0U) || (vertices == nullptr)) {
    return false;
  }

  *outMesh = GpuMesh{};

  if (!initialize_render_device()) {
    return false;
  }

  const RenderDevice *dev = render_device();
  if (!mesh_upload_device_ready(dev)) {
    return false;
  }
  if ((indexCount > 0U) && (indices == nullptr)) {
    return false;
  }
  const std::size_t strideFloats =
      hasUVs ? kVertexStrideV2Floats : kVertexStrideV1Floats;
  const std::int32_t stride =
      static_cast<std::int32_t>(strideFloats * sizeof(float));

  GpuMesh mesh{};
  mesh.hasUVs = hasUVs;
  mesh.vertexArray = dev->create_vertex_array();
  if (mesh.vertexArray == 0U) {
    return fail_mesh_upload(dev, &mesh, outMesh);
  }
  dev->bind_vertex_array(mesh.vertexArray);

  mesh.vertexBuffer = dev->create_buffer();
  if (mesh.vertexBuffer == 0U) {
    return fail_mesh_upload(dev, &mesh, outMesh);
  }
  dev->bind_array_buffer(mesh.vertexBuffer);
  dev->buffer_data_array(
      vertices,
      static_cast<std::ptrdiff_t>(vertexCount * strideFloats * sizeof(float)));

  dev->enable_vertex_attrib(0U);
  dev->vertex_attrib_float(0U, 3, stride, nullptr);

  dev->enable_vertex_attrib(1U);
  dev->vertex_attrib_float(1U, 3, stride,
                           reinterpret_cast<const void *>(sizeof(float) * 3U));

  if (hasUVs) {
    dev->enable_vertex_attrib(2U);
    dev->vertex_attrib_float(
        2U, 2, stride, reinterpret_cast<const void *>(sizeof(float) * 6U));
  }

  if ((indexCount > 0U) && (indices != nullptr)) {
    mesh.indexBuffer = dev->create_buffer();
    if (mesh.indexBuffer == 0U) {
      return fail_mesh_upload(dev, &mesh, outMesh);
    }
    dev->bind_element_buffer(mesh.indexBuffer);
    dev->buffer_data_element(indices, static_cast<std::ptrdiff_t>(
                                          indexCount * sizeof(std::uint32_t)));
  }

  unbind_mesh_upload_state(dev);

  mesh.vertexCount = vertexCount;
  mesh.indexCount = indexCount;
  *outMesh = mesh;
  return true;
}

// Precondition: caller must own the GL context before calling this function.
void unload_mesh(GpuMesh *mesh) noexcept {
  if (mesh == nullptr) {
    return;
  }

  if ((mesh->vertexArray == 0U) && (mesh->vertexBuffer == 0U) &&
      (mesh->indexBuffer == 0U)) {
    *mesh = GpuMesh{};
    return;
  }

  const RenderDevice *dev = render_device();
  if (dev != nullptr) {
    delete_mesh_resources(dev, mesh);
  } else {
    *mesh = GpuMesh{};
  }
}

/// Handles register gpu mesh.
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

/// Handles lookup gpu mesh.
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
