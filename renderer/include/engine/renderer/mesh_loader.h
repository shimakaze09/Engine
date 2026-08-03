// Declares mesh loader types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <new>

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

/// Fixed table mapping MeshHandle to uploaded GpuMesh slots.
struct GpuMeshRegistry final {
  static constexpr std::size_t kMaxSlots = 4096U;
  std::array<GpuMesh, kMaxSlots> meshes{};
  std::array<bool, kMaxSlots> occupied{};
};

/// Move-only owned buffer whose element count is bound to its allocation:
/// allocate/assign are the only growth paths and report failure instead of
/// terminating, so recoverable out-of-memory keeps normal load-failure
/// semantics under the no-exception build while the buffer stays the single
/// source of allocation truth (audit H-11 follow-up).
template <typename T>
class MeshBuffer final {
 public:
  MeshBuffer() = default;
  MeshBuffer(MeshBuffer &&) noexcept = default;
  MeshBuffer &operator=(MeshBuffer &&) noexcept = default;
  MeshBuffer(const MeshBuffer &) = delete;
  MeshBuffer &operator=(const MeshBuffer &) = delete;
  ~MeshBuffer() = default;

  /// Replaces the contents with count zero-initialized elements; false and
  /// empty on allocation failure.
  [[nodiscard]] bool allocate(std::size_t count) noexcept {
    m_data.reset();
    m_count = 0U;
    if (count == 0U) {
      return true;
    }
    m_data.reset(new (std::nothrow) T[count]{});
    if (m_data == nullptr) {
      return false;
    }
    m_count = count;
    return true;
  }

  /// Replaces the contents with a copy of values; false and empty on
  /// allocation failure.
  [[nodiscard]] bool assign(std::initializer_list<T> values) noexcept {
    if (!allocate(values.size())) {
      return false;
    }
    std::size_t index = 0U;
    for (const T &value : values) {
      m_data[index] = value;
      ++index;
    }
    return true;
  }

  void clear() noexcept {
    m_data.reset();
    m_count = 0U;
  }

  std::size_t size() const noexcept { return m_count; }
  bool empty() const noexcept { return m_count == 0U; }
  T *data() noexcept { return m_data.get(); }
  const T *data() const noexcept { return m_data.get(); }
  T &operator[](std::size_t index) noexcept { return m_data[index]; }
  const T &operator[](std::size_t index) const noexcept {
    return m_data[index];
  }

 private:
  std::unique_ptr<T[]> m_data{};
  std::size_t m_count = 0U;
};

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

// Returns slot index (same as MeshHandle::id) or 0 on failure.
std::uint32_t register_gpu_mesh(GpuMeshRegistry *registry,
                                const GpuMesh &mesh) noexcept;
/// Registry entry for a handle; nullptr when stale or absent.
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *registry,
                               renderer::MeshHandle handle) noexcept;

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
