// Declares a TriMesh triangle carried into world space as a convex shape
// GJK can use: the narrow phase, CCD and the scene queries all meet a mesh
// one triangle at a time through it.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/aabb.h"
#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/tri_mesh.h"

namespace engine::physics {

/// One mesh triangle in world space.
struct WorldTriangle final {
  math::Vec3 v[3]{};
  math::Vec3 center{};
  /// Unit face normal; its side is the caller's to choose.
  math::Vec3 normal{};
};

/// Triangle `triangle` of the mesh `meshGeometry` collides with, in world
/// space; false for one that has no area once transformed.
bool world_triangle(const ColliderWorldGeometry &meshGeometry,
                    std::uint32_t triangle, WorldTriangle *out) noexcept;

/// GJK support of a WorldTriangle (`data`).
math::Vec3 support_world_triangle(const void *data, const math::Vec3 &center,
                                  const math::Vec3 &direction) noexcept;

/// The box in the mesh's local space around the world box `box`, for
/// finding the triangles near something in world space.
math::AABB world_box_into_mesh(const ColliderWorldGeometry &meshGeometry,
                               const math::AABB &box) noexcept;

/// Triangles one pair or one sweep considers. A collider over more than
/// this many keeps the lowest-numbered ones.
inline constexpr std::size_t kMaxTriMeshCandidates = 256U;

/// `meshGeometry` narrowed to its triangle `triangle`: a convex piece with
/// that triangle's support, world bounds and centroid. False for a
/// triangle with no area or one the mesh does not have.
bool tri_mesh_piece(const ColliderWorldGeometry &meshGeometry,
                    std::uint32_t triangle,
                    ColliderWorldGeometry *outPiece) noexcept;

/// Calls `visit(piece)` for each convex piece of `shape` that may meet the
/// world box `worldBox`: the shape itself, or for a whole TriMesh each of
/// its triangles there, lowest first (at most kMaxTriMeshCandidates).
/// Stops when `visit` returns false. Every query that tests a collider
/// with GJK goes through this, so a mesh is met by its triangles and
/// never by its bounds.
template <typename Visit>
void for_each_shape_piece(const ColliderWorldGeometry &shape,
                          const math::AABB &worldBox, Visit visit) noexcept {
  if ((shape.shape != math::ColliderShape::TriMesh) ||
      (shape.triangle != kWholeShape) || (shape.triMesh == nullptr)) {
    static_cast<void>(visit(shape));
    return;
  }
  std::uint32_t triangles[kMaxTriMeshCandidates] = {};
  const std::size_t found = collect_tri_mesh_triangles(
      *shape.triMesh, world_box_into_mesh(shape, worldBox), triangles,
      kMaxTriMeshCandidates);
  const std::size_t count =
      (found < kMaxTriMeshCandidates) ? found : kMaxTriMeshCandidates;
  ColliderWorldGeometry piece{};
  for (std::size_t i = 0U; i < count; ++i) {
    if (tri_mesh_piece(shape, triangles[i], &piece) && !visit(piece)) {
      return;
    }
  }
}

} // namespace engine::physics
