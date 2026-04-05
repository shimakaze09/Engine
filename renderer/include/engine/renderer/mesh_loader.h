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

} // namespace engine::renderer