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

#include "engine/renderer/asset_staleness.h"
#include "mesh_handle_codec.h"

namespace engine::renderer {

namespace {

constexpr std::size_t kVertexStrideV1Floats = 6U;
constexpr std::size_t kVertexStrideV2Floats = 8U;
constexpr std::size_t kVertexStrideV3Floats = 16U;

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

  if ((mesh->geometry != kInvalidDeviceGeometry) &&
      (dev->destroy_geometry != nullptr)) {
    dev->destroy_geometry(mesh->geometry);
  }
  if ((mesh->indexBuffer != kInvalidDeviceBuffer) &&
      (dev->destroy_buffer != nullptr)) {
    dev->destroy_buffer(mesh->indexBuffer);
  }
  if ((mesh->vertexBuffer != kInvalidDeviceBuffer) &&
      (dev->destroy_buffer != nullptr)) {
    dev->destroy_buffer(mesh->vertexBuffer);
  }
  *mesh = GpuMesh{};
}

bool mesh_upload_device_ready(const RenderDevice *dev) noexcept {
  return (dev != nullptr) && (dev->create_buffer != nullptr) &&
         (dev->destroy_buffer != nullptr) &&
         (dev->create_geometry != nullptr) &&
         (dev->destroy_geometry != nullptr);
}

/// Builds the interleaved-float layout matching the mesh's UV/skin shape.
VertexLayout mesh_vertex_layout(bool hasUVs, bool hasSkin,
                                std::size_t strideFloats) noexcept {
  VertexLayout layout{};
  layout.strideBytes =
      static_cast<std::int32_t>(strideFloats * sizeof(float));
  layout.attributes[0] = {VertexSemantic::Position, 3, 0};
  layout.attributes[1] = {VertexSemantic::Normal, 3,
                          static_cast<std::int32_t>(sizeof(float) * 3U)};
  layout.attributeCount = 2U;
  if (hasUVs) {
    layout.attributes[layout.attributeCount] = {
        VertexSemantic::TexCoord0, 2,
        static_cast<std::int32_t>(sizeof(float) * 6U)};
    ++layout.attributeCount;
  }
  if (hasSkin) {
    layout.attributes[layout.attributeCount] = {
        VertexSemantic::Joints, 4,
        static_cast<std::int32_t>(sizeof(float) * kSkinJointOffsetFloats)};
    ++layout.attributeCount;
    layout.attributes[layout.attributeCount] = {
        VertexSemantic::Weights, 4,
        static_cast<std::int32_t>(sizeof(float) * kSkinWeightOffsetFloats)};
    ++layout.attributeCount;
  }
  return layout;
}

bool fail_mesh_upload(const RenderDevice *dev, GpuMesh *mesh,
                      GpuMesh *outMesh) noexcept {
  delete_mesh_resources(dev, mesh);
  if (outMesh != nullptr) {
    *outMesh = GpuMesh{};
  }
  core::log_message(core::LogLevel::Error, "renderer",
                    "failed to create mesh GPU resources");
  return false;
}

/// Uploads interleaved vertices (plus optional indices) as device buffers
/// and geometry; shared by the cooked-asset and in-memory entry points.
bool upload_mesh_streams(const RenderDevice *dev, const float *vertices,
                         std::size_t vertexFloatCount,
                         const std::uint32_t *indices,
                         std::size_t indexCount, bool hasUVs, bool hasSkin,
                         std::size_t strideFloats, GpuMesh *mesh,
                         GpuMesh *outMesh) noexcept {
  BufferDesc vertexDesc{};
  vertexDesc.usage = BufferUsage::Vertex;
  vertexDesc.access = BufferAccess::Static;
  vertexDesc.sizeBytes =
      static_cast<std::ptrdiff_t>(vertexFloatCount * sizeof(float));
  vertexDesc.data = vertices;
  mesh->vertexBuffer = dev->create_buffer(vertexDesc);
  if (mesh->vertexBuffer == kInvalidDeviceBuffer) {
    return fail_mesh_upload(dev, mesh, outMesh);
  }

  if (indexCount > 0U) {
    BufferDesc indexDesc{};
    indexDesc.usage = BufferUsage::Index;
    indexDesc.access = BufferAccess::Static;
    indexDesc.sizeBytes =
        static_cast<std::ptrdiff_t>(indexCount * sizeof(std::uint32_t));
    indexDesc.data = indices;
    mesh->indexBuffer = dev->create_buffer(indexDesc);
    if (mesh->indexBuffer == kInvalidDeviceBuffer) {
      return fail_mesh_upload(dev, mesh, outMesh);
    }
  }

  GeometryDesc geometryDesc{};
  geometryDesc.vertexBuffer = mesh->vertexBuffer;
  geometryDesc.layout = mesh_vertex_layout(hasUVs, hasSkin, strideFloats);
  geometryDesc.indexBuffer = mesh->indexBuffer;
  mesh->geometry = dev->create_geometry(geometryDesc);
  if (mesh->geometry == kInvalidDeviceGeometry) {
    return fail_mesh_upload(dev, mesh, outMesh);
  }
  return true;
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

  warn_if_cooked_asset_stale(path);

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

// Precondition: caller must own the render context before calling this
// function. Context acquisition and release are the engine loop's responsibility;
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

// Precondition: caller must own the render context before this call.
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

  GpuMesh mesh{};
  mesh.hasUVs = meshData.hasUVs;
  mesh.hasSkin = meshData.hasSkin;
  if (!upload_mesh_streams(dev, meshData.vertices.data(),
                           meshData.vertices.size(), meshData.indices.data(),
                           meshData.indices.size(), meshData.hasUVs,
                           meshData.hasSkin, meshData.strideFloats, &mesh,
                           outMesh)) {
    return false;
  }

  mesh.vertexCount = meshData.vertexCount;
  mesh.indexCount = static_cast<std::uint32_t>(meshData.indices.size());
  *outMesh = mesh;
  return true;
}

// Precondition: caller must own the render context before this call.
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

  GpuMesh mesh{};
  mesh.hasUVs = hasUVs;
  if (!upload_mesh_streams(dev, vertices,
                           static_cast<std::size_t>(vertexCount) * strideFloats,
                           indices, indexCount, hasUVs, false, strideFloats,
                           &mesh, outMesh)) {
    return false;
  }

  mesh.vertexCount = vertexCount;
  mesh.indexCount = indexCount;
  *outMesh = mesh;
  return true;
}

// Precondition: caller must own the render context before this call.
void unload_mesh(GpuMesh *mesh) noexcept {
  if (mesh == nullptr) {
    return;
  }

  if ((mesh->geometry == kInvalidDeviceGeometry) &&
      (mesh->vertexBuffer == kInvalidDeviceBuffer) &&
      (mesh->indexBuffer == kInvalidDeviceBuffer)) {
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

MeshHandle register_gpu_mesh(GpuMeshRegistry *registry,
                             const GpuMesh &mesh) noexcept {
  if (registry == nullptr) {
    return kInvalidMeshHandle;
  }

  for (std::size_t i = 1U; i < registry->meshes.size(); ++i) {
    if (!registry->occupied[i]) {
      registry->meshes[i] = mesh;
      registry->occupied[i] = true;
      // Slot generations start at 1 (0 is the "never used" storage default
      // and also the codec's invalid sentinel), matching TextureSlot.
      if (registry->generations[i] == 0U) {
        registry->generations[i] = 1U;
      }
      return mesh_handle_detail::make_handle(i, registry->generations[i]);
    }
  }

  return kInvalidMeshHandle;
}

const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *registry,
                               renderer::MeshHandle handle) noexcept {
  if ((registry == nullptr) || (handle == kInvalidMeshHandle)) {
    return nullptr;
  }

  const std::uint32_t slot = mesh_handle_detail::slot_index(handle);
  const std::uint32_t generation = mesh_handle_detail::generation(handle);
  if ((slot == 0U) || (slot >= registry->meshes.size()) ||
      (generation == 0U)) {
    return nullptr;
  }

  if (!registry->occupied[slot] || (registry->generations[slot] != generation)) {
    return nullptr;
  }

  return &registry->meshes[slot];
}

void unload_gpu_mesh(GpuMeshRegistry *registry, MeshHandle handle) noexcept {
  if ((registry == nullptr) || (handle == kInvalidMeshHandle)) {
    return;
  }

  const std::uint32_t slot = mesh_handle_detail::slot_index(handle);
  const std::uint32_t generation = mesh_handle_detail::generation(handle);
  if ((slot == 0U) || (slot >= registry->meshes.size()) ||
      (generation == 0U)) {
    return;
  }

  if (!registry->occupied[slot] || (registry->generations[slot] != generation)) {
    return;
  }

  unload_mesh(&registry->meshes[slot]);
  registry->occupied[slot] = false;
  registry->meshes[slot] = GpuMesh{};
  // Bump the generation on release so any handle still pointing at this
  // slot fails lookup instead of aliasing the next mesh loaded here
  // (audit #173).
  registry->generations[slot] =
      mesh_handle_detail::next_generation(registry->generations[slot]);
}

} // namespace engine::renderer
