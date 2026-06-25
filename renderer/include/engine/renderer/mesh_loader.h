// Declares mesh loader types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "engine/renderer/command_buffer.h"

namespace engine::renderer {

/// Stores gpu mesh data used by the engine.
struct GpuMesh final {
  std::uint32_t vertexArray = 0U;
  std::uint32_t vertexBuffer = 0U;
  std::uint32_t indexBuffer = 0U;
  std::uint32_t vertexCount = 0U;
  std::uint32_t indexCount = 0U;
  bool hasUVs = false;
};

/// Stores gpu mesh registry data used by the engine.
struct GpuMeshRegistry final {
  static constexpr std::size_t kMaxSlots = 4096U;
  std::array<GpuMesh, kMaxSlots> meshes{};
  std::array<bool, kMaxSlots> occupied{};
};

/// CPU-side mesh payload decoded from a cooked mesh asset file.
struct CpuMeshData final {
  std::unique_ptr<float[]> vertices{};
  std::unique_ptr<std::uint32_t[]> indices{};
  std::uint32_t vertexCount = 0U;
  std::uint32_t indexCount = 0U;
  std::size_t vertexFloatCount = 0U;
  std::size_t strideFloats = 6U;
  bool hasUVs = false;
};

// Returns slot index (same as MeshHandle::id) or 0 on failure.
std::uint32_t register_gpu_mesh(GpuMeshRegistry *registry,
                                const GpuMesh &mesh) noexcept;
/// Handles lookup gpu mesh.
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *registry,
                               renderer::MeshHandle handle) noexcept;

/// Loads the requested resource for mesh from file.
bool load_mesh_from_file(const char *path, GpuMesh *outMesh) noexcept;
/// Decodes a cooked mesh file without touching GPU state.
bool load_mesh_data_from_file(const char *path, CpuMeshData *outData,
                              std::uint64_t *outSizeBytes = nullptr) noexcept;
/// Uploads decoded CPU mesh data to the current render context.
bool upload_mesh_data_to_gpu(const CpuMeshData &meshData,
                             GpuMesh *outMesh) noexcept;
/// Handles unload mesh.
void unload_mesh(GpuMesh *mesh) noexcept;

// Direct GPU upload from in-memory vertex/index data (no file I/O).
// Precondition: caller must own the GL context before calling this function.
// vertices: array of floats, 6 per vertex (pos xyz, norm xyz) when !hasUVs,
//           8 per vertex (pos xyz, norm xyz, uv xy) when hasUVs.
// indices: may be nullptr when indexCount == 0 (drawArrays path).
bool build_gpu_mesh_from_data(const float *vertices, std::uint32_t vertexCount,
                              const std::uint32_t *indices,
                              std::uint32_t indexCount, bool hasUVs,
                              GpuMesh *outMesh) noexcept;

} // namespace engine::renderer
