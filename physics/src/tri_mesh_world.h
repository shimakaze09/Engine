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

} // namespace engine::physics
