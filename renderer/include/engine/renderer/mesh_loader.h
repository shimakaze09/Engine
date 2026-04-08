#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/renderer/command_buffer.h"

namespace engine::renderer {

struct GpuMesh final {
  std::uint32_t vertexArray = 0U;
  std::uint32_t vertexBuffer = 0U;
  std::uint32_t indexBuffer = 0U;
  std::uint32_t vertexCount = 0U;
  std::uint32_t indexCount = 0U;
  bool hasUVs = false;
};

struct GpuMeshRegistry final {
  static constexpr std::size_t kMaxSlots = 4096U;
  std::array<GpuMesh, kMaxSlots> meshes{};
  std::array<bool, kMaxSlots> occupied{};
};

// Returns slot index (same as MeshHandle::id) or 0 on failure.
std::uint32_t register_gpu_mesh(GpuMeshRegistry *registry,
                                const GpuMesh &mesh) noexcept;
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *registry,
                               renderer::MeshHandle handle) noexcept;

bool load_mesh_from_file(const char *path, GpuMesh *outMesh) noexcept;
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