// Implements carrying TriMesh triangles and boxes between a mesh's local
// space and the world, for the narrow phase, CCD and the scene queries.

#include "tri_mesh_world.h"

#include <algorithm>
#include <cmath>

#include "engine/math/vec4.h"

namespace engine::physics {

bool world_triangle(const ColliderWorldGeometry &meshGeometry,
                    std::uint32_t triangle, WorldTriangle *out) noexcept {
  const TriMeshData *mesh = meshGeometry.triMesh;
  if ((mesh == nullptr) || (out == nullptr) ||
      (triangle >= mesh->triangle_count())) {
    return false;
  }
  for (std::size_t k = 0U; k < 3U; ++k) {
    out->v[k] = math::transform_point(meshGeometry.localToWorld,
                                      mesh->corner(triangle, k));
  }
  out->center = math::mul(math::add(math::add(out->v[0], out->v[1]), out->v[2]),
                          1.0F / 3.0F);
  const math::Vec3 normal = math::cross(math::sub(out->v[1], out->v[0]),
                                        math::sub(out->v[2], out->v[0]));
  const float length = math::length(normal);
  if (!(length > 0.0F) || !std::isfinite(length)) {
    return false;
  }
  out->normal = math::mul(normal, 1.0F / length);
  return true;
}

math::Vec3 support_world_triangle(const void *data,
                                  const math::Vec3 & /*center*/,
                                  const math::Vec3 &direction) noexcept {
  const auto *triangle = static_cast<const WorldTriangle *>(data);
  std::size_t best = 0U;
  float bestProjection = math::dot(triangle->v[0], direction);
  for (std::size_t i = 1U; i < 3U; ++i) {
    const float projection = math::dot(triangle->v[i], direction);
    if (projection > bestProjection) {
      bestProjection = projection;
      best = i;
    }
  }
  return triangle->v[best];
}

bool tri_mesh_piece(const ColliderWorldGeometry &meshGeometry,
                    std::uint32_t triangle,
                    ColliderWorldGeometry *outPiece) noexcept {
  WorldTriangle corners{};
  if ((outPiece == nullptr) ||
      !world_triangle(meshGeometry, triangle, &corners)) {
    return false;
  }
  *outPiece = meshGeometry;
  outPiece->triangle = triangle;
  outPiece->center = corners.center;
  outPiece->worldAabb = math::AABB{corners.v[0], corners.v[0]};
  for (std::size_t k = 1U; k < 3U; ++k) {
    math::AABB &box = outPiece->worldAabb;
    box.min = math::Vec3(std::min(box.min.x, corners.v[k].x),
                         std::min(box.min.y, corners.v[k].y),
                         std::min(box.min.z, corners.v[k].z));
    box.max = math::Vec3(std::max(box.max.x, corners.v[k].x),
                         std::max(box.max.y, corners.v[k].y),
                         std::max(box.max.z, corners.v[k].z));
  }
  return true;
}

math::AABB world_box_into_mesh(const ColliderWorldGeometry &meshGeometry,
                               const math::AABB &box) noexcept {
  math::AABB local{};
  for (int corner = 0; corner < 8; ++corner) {
    const math::Vec3 world((corner & 1) ? box.max.x : box.min.x,
                           (corner & 2) ? box.max.y : box.min.y,
                           (corner & 4) ? box.max.z : box.min.z);
    const math::Vec3 p =
        math::transform_point(meshGeometry.worldToLocal, world);
    if (corner == 0) {
      local = math::AABB{p, p};
      continue;
    }
    local.min =
        math::Vec3(std::min(local.min.x, p.x), std::min(local.min.y, p.y),
                   std::min(local.min.z, p.z));
    local.max =
        math::Vec3(std::max(local.max.x, p.x), std::max(local.max.y, p.y),
                   std::max(local.max.z, p.z));
  }
  return local;
}

} // namespace engine::physics
