// Declares mesh loader types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/nothrow_buffer.h"
#include "engine/renderer/command_buffer.h"

namespace engine::renderer {

/// Uploaded mesh: VAO/VBO/EBO ids and index count.
struct GpuMesh final {
  std::uint32_t vertexArray = 0U;
  std::uint32_t vertexBuffer = 0U;
  std::uint32_t indexBuffer = 0U;
  std::uint32_t vertexCount = 0U;
  std::uint32_t indexCount = 0U;
  bool hasUVs = false;
  bool hasSkin = false;
};

/// Fixed table mapping MeshHandle to uploaded GpuMesh slots. Each slot
/// carries a generation counter (bumped on release) so a MeshHandle that
/// outlives its slot's reuse fails lookup instead of aliasing whatever mesh
/// was loaded into the recycled slot afterward (audit #173).
struct GpuMeshRegistry final {
  static constexpr std::size_t kMaxSlots = 4096U;
  std::array<GpuMesh, kMaxSlots> meshes{};
  std::array<bool, kMaxSlots> occupied{};
  std::array<std::uint32_t, kMaxSlots> generations{};
};

/// Mesh-domain alias for the shared nothrow buffer (audit H-11 follow-up;
/// generalized to core::NothrowBuffer under audit #174 so runtime's
/// animation payload loader can reuse the same allocate/assign contract).
template <typename T>
using MeshBuffer = core::NothrowBuffer<T>;

/// CPU-side mesh payload decoded from a cooked mesh asset file. The
/// buffers are the single source of allocation truth: float and index
/// counts derive from their sizes, so metadata can never claim more data
/// than is owned, and the move-only buffers make an accidental hot-path
/// copy a compile error (review follow-up to audit H-11 — the former
/// raw-array fields carried independently editable counts with no
/// capacity).
struct CpuMeshData final {
  MeshBuffer<float> vertices{};
  MeshBuffer<std::uint32_t> indices{};
  std::uint32_t vertexCount = 0U;
  std::size_t strideFloats = 6U;
  bool hasUVs = false;
  bool hasSkin = false;
};

/// Claims a free slot and returns a generation-encoded handle, or
/// kInvalidMeshHandle when the registry is full.
MeshHandle register_gpu_mesh(GpuMeshRegistry *registry,
                             const GpuMesh &mesh) noexcept;
/// Registry entry for a handle; nullptr when stale (slot reused under a
/// newer generation) or absent.
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *registry,
                               renderer::MeshHandle handle) noexcept;
/// Releases the slot backing handle (no-op on a stale or absent handle) and
/// bumps its generation so any surviving copy of handle fails lookup.
void unload_gpu_mesh(GpuMeshRegistry *registry, MeshHandle handle) noexcept;

/// Loads the requested resource for mesh from file.
bool load_mesh_from_file(const char *path, GpuMesh *outMesh) noexcept;
/// Decodes a cooked mesh file without touching GPU state.
bool load_mesh_data_from_file(const char *path, CpuMeshData *outData,
                              std::uint64_t *outSizeBytes = nullptr) noexcept;
/// Validates a CpuMeshData payload against its declared layout: stride
/// matches the UV/skin flags exactly (6/8/16 floats), vertices.size()
/// equals vertexCount times stride under checked multiplication, indices
/// (an empty buffer selects the non-indexed drawArrays path) address live
/// vertices, and skinned joint indices are finite and inside the bone
/// palette with finite weights (audit H-11).
bool mesh_data_valid(const CpuMeshData &meshData) noexcept;
/// Uploads decoded CPU mesh data to the current render context.
bool upload_mesh_data_to_gpu(const CpuMeshData &meshData,
                             GpuMesh *outMesh) noexcept;
/// Deletes the mesh's GL buffers and clears the struct.
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
