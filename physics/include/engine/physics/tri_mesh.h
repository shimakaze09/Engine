// Declares the triangle mesh a TriMesh collider collides with: its vertices
// and triangles, a bounding volume hierarchy over them, and the queries the
// narrow phase and the scene queries run against it. A mesh is immutable
// once built and shared by reference count, so every collider that uses
// the same mesh, and every copy of a world, shares one copy of it.
//
// A triangle mesh has no volume and no inside, so like Unity's concave
// MeshCollider, Godot's ConcavePolygonShape3D and Jolt's MeshShape it is
// only ever static geometry: a body never moves on one.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "engine/math/aabb.h"
#include "engine/math/vec3.h"

namespace engine::physics {

/// Three indices into a mesh's vertices.
struct TriMeshTriangle final {
  std::uint32_t v[3] = {0U, 0U, 0U};
};

/// One BVH node. A leaf (`count` > 0) holds triangles [first, first +
/// count); an interior node's children are the next node and node `first`.
struct TriMeshNode final {
  math::AABB bounds{};
  std::uint32_t first = 0U;
  std::uint32_t count = 0U;
};

/// Why a mesh did not build.
enum class TriMeshBuildResult : std::uint8_t {
  Ok,
  /// No triangles, or an index count that is not a multiple of three.
  Empty,
  TooManyVertices,
  TooManyTriangles,
  IndexOutOfRange,
  NonFinite,
  /// Every triangle had no area.
  AllDegenerate,
  OutOfMemory,
};

class TriMeshRef;

/// A built, immutable triangle mesh in its collider's local space.
class TriMeshData final {
public:
  /// A level mesh larger than this is split into several colliders.
  static constexpr std::size_t kMaxTriangles = 1U << 20U;
  static constexpr std::size_t kMaxVertices = 1U << 21U;
  /// Triangles per BVH leaf.
  static constexpr std::size_t kLeafTriangles = 4U;

  TriMeshData() noexcept = default;
  ~TriMeshData();
  TriMeshData(const TriMeshData &) = delete;
  TriMeshData &operator=(const TriMeshData &) = delete;

  const math::Vec3 *vertices() const noexcept { return vertices_; }
  std::size_t vertex_count() const noexcept { return vertexCount_; }
  /// The kept triangles in BVH order; a triangle's index is its place here.
  const TriMeshTriangle *triangles() const noexcept { return triangles_; }
  std::size_t triangle_count() const noexcept { return triangleCount_; }
  const TriMeshNode *nodes() const noexcept { return nodes_; }
  std::size_t node_count() const noexcept { return nodeCount_; }
  /// Bounds of every kept triangle.
  const math::AABB &local_bounds() const noexcept { return localBounds_; }
  /// Triangles of the source dropped because they had no area.
  std::size_t dropped_degenerate() const noexcept { return droppedDegenerate_; }

  /// Corner `corner` (0-2) of triangle `triangle`.
  const math::Vec3 &corner(std::size_t triangle,
                           std::size_t corner) const noexcept {
    return vertices_[triangles_[triangle].v[corner]];
  }

private:
  friend class TriMeshRef;
  friend TriMeshBuildResult build_tri_mesh(const math::Vec3 *vertices,
                                           std::size_t vertexCount,
                                           const std::uint32_t *indices,
                                           std::size_t indexCount,
                                           TriMeshRef *outMesh) noexcept;

  math::Vec3 *vertices_ = nullptr;
  std::size_t vertexCount_ = 0U;
  TriMeshTriangle *triangles_ = nullptr;
  std::size_t triangleCount_ = 0U;
  TriMeshNode *nodes_ = nullptr;
  std::size_t nodeCount_ = 0U;
  std::size_t nodeCapacity_ = 0U;
  math::AABB localBounds_{};
  std::size_t droppedDegenerate_ = 0U;
  std::atomic<std::uint32_t> references_{0U};
};

/// A counted reference to a built mesh. Copying it shares the mesh; the
/// last reference to go frees it.
class TriMeshRef final {
public:
  TriMeshRef() noexcept = default;
  ~TriMeshRef();
  TriMeshRef(const TriMeshRef &other) noexcept;
  TriMeshRef(TriMeshRef &&other) noexcept;
  TriMeshRef &operator=(const TriMeshRef &other) noexcept;
  TriMeshRef &operator=(TriMeshRef &&other) noexcept;

  const TriMeshData *get() const noexcept { return data_; }
  explicit operator bool() const noexcept { return data_ != nullptr; }
  /// Drops this reference.
  void reset() noexcept;

private:
  friend struct TriMeshBuilder;
  explicit TriMeshRef(TriMeshData *adopted) noexcept;

  TriMeshData *data_ = nullptr;
};

/// A short description of `result` for a diagnostic.
const char *tri_mesh_build_result_text(TriMeshBuildResult result) noexcept;

/// Builds a mesh from `indexCount / 3` triangles over `vertices`, dropping
/// the triangles with no area. The BVH splits each node at the median of
/// its triangles' centroids along its longest axis, ties broken by source
/// order, so the same input builds the same mesh on every platform.
/// `*outMesh` is left unchanged unless the result is Ok.
TriMeshBuildResult build_tri_mesh(const math::Vec3 *vertices,
                                  std::size_t vertexCount,
                                  const std::uint32_t *indices,
                                  std::size_t indexCount,
                                  TriMeshRef *outMesh) noexcept;

/// Writes into `outTriangles`, in ascending order, the triangles whose
/// bounds meet `localBox`, at most `maxTriangles`; returns how many meet
/// it, which may exceed `maxTriangles`. Nothing is allocated.
std::size_t collect_tri_mesh_triangles(const TriMeshData &mesh,
                                       const math::AABB &localBox,
                                       std::uint32_t *outTriangles,
                                       std::size_t maxTriangles) noexcept;

/// The nearest triangle a ray meets.
struct TriMeshRayHit final {
  /// Distance along the ray in units of its direction's length.
  float t = 0.0F;
  std::uint32_t triangle = 0U;
  /// Unit face normal, turned to face the ray's origin.
  math::Vec3 normal{};
};

/// Casts the ray `origin + t * direction`, 0 <= t <= maxT, in the mesh's
/// local space, from either side of a triangle. The nearest hit wins, and
/// of two at the same distance the lower triangle. False when it meets
/// none.
bool raycast_tri_mesh(const TriMeshData &mesh, const math::Vec3 &origin,
                      const math::Vec3 &direction, float maxT,
                      TriMeshRayHit *outHit) noexcept;

/// Whether the ray `origin + t * direction`, 0 <= t <= maxT, meets the
/// triangle (a, b, c) from either side; `*outT` receives t (Moller and
/// Trumbore).
bool ray_triangle_intersect(const math::Vec3 &origin,
                            const math::Vec3 &direction, const math::Vec3 &a,
                            const math::Vec3 &b, const math::Vec3 &c,
                            float maxT, float *outT) noexcept;

/// The point of the triangle (a, b, c) nearest `p`.
math::Vec3 closest_point_on_triangle(const math::Vec3 &p, const math::Vec3 &a,
                                     const math::Vec3 &b,
                                     const math::Vec3 &c) noexcept;

/// The point of triangle `triangle` nearest `point`.
math::Vec3 closest_point_on_tri_mesh_triangle(const TriMeshData &mesh,
                                              std::uint32_t triangle,
                                              const math::Vec3 &point) noexcept;

} // namespace engine::physics
