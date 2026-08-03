// Implements mesh loader behavior for the Engine renderer system.

#include "engine/renderer/mesh_loader.h"

#include <cmath>
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
constexpr std::size_t kVertexStrideV3Floats = 16U;

/// Vertex attribute locations for skinned joint indices and weights;
/// 3-7 are reserved for the per-instance model matrix and foliage data.
constexpr std::uint32_t kSkinJointsAttrib = 8U;
constexpr std::uint32_t kSkinWeightsAttrib = 9U;

/// Float offsets of the joint-index and weight attributes inside a
/// stride-16 skinned vertex (position 0-2, normal 3-5, uv 6-7).
constexpr std::size_t kSkinJointOffsetFloats = 8U;
constexpr std::size_t kSkinWeightOffsetFloats = 12U;
constexpr std::uint32_t kMaxMeshVertexCount = 1000000U;
constexpr std::uint32_t kMaxMeshIndexCount = 3000000U;

/// Reads exact data.
bool read_exact(FILE *file, void *data, std::size_t sizeBytes) noexcept {
  if ((file == nullptr) || (data == nullptr) || (sizeBytes == 0U)) {
    return false;
  }

  return std::fread(data, 1U, sizeBytes, file) == sizeBytes;
}

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

/// Returns whether every mesh index addresses an existing vertex.
bool mesh_indices_in_range(const std::uint32_t *indices,
                           std::uint32_t indexCount,
                           std::uint32_t vertexCount) noexcept {
  if (indexCount == 0U) {
    return true;
  }
  if ((indices == nullptr) || (vertexCount == 0U)) {
    return false;
  }

  for (std::uint32_t index = 0U; index < indexCount; ++index) {
    if (indices[index] >= vertexCount) {
      return false;
    }
  }
  return true;
}

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
  const bool isV3 = (header.version == core::kMeshAssetVersion3);
  if (!isV1 && !isV2 && !isV3) {
    std::fclose(file);
    return false;
  }

  const std::size_t strideFloats =
      isV3 ? kVertexStrideV3Floats
           : (isV2 ? kVertexStrideV2Floats : kVertexStrideV1Floats);

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
  // The buffers are the allocation truth for every later bounds check, and
  // allocation failure stays a recoverable load failure.
  CpuMeshData decoded{};
  if (!decoded.vertices.allocate(vertexFloatCount)) {
    std::fclose(file);
    return false;
  }
  if (vertexFloatCount > 0U) {
    if (!read_exact(file, decoded.vertices.data(), vertexBytes)) {
      std::fclose(file);
      return false;
    }
  }

  const std::size_t indexCount = static_cast<std::size_t>(header.indexCount);
  if (!decoded.indices.allocate(indexCount)) {
    std::fclose(file);
    return false;
  }
  if (indexCount > 0U) {
    if (!read_exact(file, decoded.indices.data(), indexBytes)) {
      std::fclose(file);
      return false;
    }

    if (!mesh_indices_in_range(decoded.indices.data(), header.indexCount,
                               header.vertexCount)) {
      std::fclose(file);
      return false;
    }
  }

  std::fclose(file);

  decoded.vertexCount = header.vertexCount;
  decoded.strideFloats = strideFloats;
  decoded.hasUVs = isV2 || isV3;
  decoded.hasSkin = isV3;
  if (!mesh_data_valid(decoded)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "mesh asset payload failed validation");
    return false;
  }

  *outData = std::move(decoded);
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

bool mesh_data_valid(const CpuMeshData &meshData) noexcept {
  if ((meshData.vertexCount == 0U) || meshData.vertices.empty()) {
    return false;
  }

  // Skinned vertices carry joints/weights behind the UV slot, so a skin
  // flag without UVs would misdeclare the attribute offsets.
  if (meshData.hasSkin && !meshData.hasUVs) {
    return false;
  }
  std::size_t expectedStride = kVertexStrideV1Floats;
  if (meshData.hasSkin) {
    expectedStride = kVertexStrideV3Floats;
  } else if (meshData.hasUVs) {
    expectedStride = kVertexStrideV2Floats;
  }
  if (meshData.strideFloats != expectedStride) {
    return false;
  }

  // The vector size IS the allocation, so this equality proves every
  // per-vertex access below (and the GPU upload) stays in bounds.
  std::size_t expectedFloatCount = 0U;
  if (!checked_mul(static_cast<std::size_t>(meshData.vertexCount),
                   meshData.strideFloats, &expectedFloatCount) ||
      (meshData.vertices.size() != expectedFloatCount)) {
    return false;
  }

  if (meshData.indices.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return false;
  }
  if (!mesh_indices_in_range(
          meshData.indices.data(),
          static_cast<std::uint32_t>(meshData.indices.size()),
          meshData.vertexCount)) {
    return false;
  }

  if (meshData.hasSkin) {
    const float *vertices = meshData.vertices.data();
    const float paletteBound = static_cast<float>(kMaxSkinPaletteJoints);
    for (std::size_t vertex = 0U; vertex < meshData.vertexCount; ++vertex) {
      const float *joints =
          vertices + (vertex * meshData.strideFloats) + kSkinJointOffsetFloats;
      const float *weights =
          vertices + (vertex * meshData.strideFloats) + kSkinWeightOffsetFloats;
      for (std::size_t component = 0U; component < 4U; ++component) {
        if (!std::isfinite(joints[component]) || (joints[component] < 0.0F) ||
            (joints[component] >= paletteBound)) {
          return false;
        }
        if (!std::isfinite(weights[component])) {
          return false;
        }
      }
    }
  }
  return true;
}

// Precondition: caller must own the GL context before calling this function.
bool upload_mesh_data_to_gpu(const CpuMeshData &meshData,
                             GpuMesh *outMesh) noexcept {
  if (outMesh == nullptr) {
    return false;
  }

  *outMesh = GpuMesh{};

  if (!mesh_data_valid(meshData)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "mesh upload rejected inconsistent CpuMeshData");
    return false;
  }

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
  mesh.hasSkin = meshData.hasSkin;
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
      meshData.vertices.data(),
      static_cast<std::ptrdiff_t>(meshData.vertices.size() * sizeof(float)));

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

  if (meshData.hasSkin) {
    dev->enable_vertex_attrib(kSkinJointsAttrib);
    dev->vertex_attrib_float(
        kSkinJointsAttrib, 4, stride,
        reinterpret_cast<const void *>(sizeof(float) * 8U));
    dev->enable_vertex_attrib(kSkinWeightsAttrib);
    dev->vertex_attrib_float(
        kSkinWeightsAttrib, 4, stride,
        reinterpret_cast<const void *>(sizeof(float) * 12U));
  }

  if (!meshData.indices.empty()) {
    mesh.indexBuffer = dev->create_buffer();
    if (mesh.indexBuffer == 0U) {
      return fail_mesh_upload(dev, &mesh, outMesh);
    }
    dev->bind_element_buffer(mesh.indexBuffer);
    dev->buffer_data_element(
        meshData.indices.data(),
        static_cast<std::ptrdiff_t>(meshData.indices.size() *
                                    sizeof(std::uint32_t)));
  }

  unbind_mesh_upload_state(dev);

  mesh.vertexCount = meshData.vertexCount;
  mesh.indexCount = static_cast<std::uint32_t>(meshData.indices.size());
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

  if (!mesh_indices_in_range(indices, indexCount, vertexCount)) {
    return false;
  }

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
