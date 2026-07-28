// Builds affine world geometry and support mappings for physics colliders.

#include "engine/physics/collider.h"

#include "engine/math/transform.h"
#include "engine/math/vec4.h"

#include <cmath>
#include <cstddef>

namespace engine::physics {
namespace {

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool finite(const math::Vec3 &value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(const math::Quat &value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z) &&
         finite(value.w);
}

[[nodiscard]] bool finite(const math::Mat4 &value) noexcept {
  for (const math::Vec4 &column : value.columns) {
    if (!finite(column.x) || !finite(column.y) || !finite(column.z) ||
        !finite(column.w)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool affine(const math::Mat4 &value) noexcept {
  constexpr float epsilon = 1.0e-6F;
  return std::fabs(value.columns[0].w) <= epsilon &&
         std::fabs(value.columns[1].w) <= epsilon &&
         std::fabs(value.columns[2].w) <= epsilon &&
         std::fabs(value.columns[3].w - 1.0F) <= epsilon;
}

[[nodiscard]] math::Vec3 transform_point(const math::Mat4 &matrix,
                                         const math::Vec3 &point) noexcept {
  const math::Vec4 result =
      math::mul(matrix, math::Vec4(point.x, point.y, point.z, 1.0F));
  return math::Vec3(result.x, result.y, result.z);
}

[[nodiscard]] math::Vec3
support_direction_local(const math::Mat4 &localToWorld,
                        const math::Vec3 &worldDirection) noexcept {
  return math::Vec3(worldDirection.x * localToWorld.columns[0].x +
                        worldDirection.y * localToWorld.columns[0].y +
                        worldDirection.z * localToWorld.columns[0].z,
                    worldDirection.x * localToWorld.columns[1].x +
                        worldDirection.y * localToWorld.columns[1].y +
                        worldDirection.z * localToWorld.columns[1].z,
                    worldDirection.x * localToWorld.columns[2].x +
                        worldDirection.y * localToWorld.columns[2].y +
                        worldDirection.z * localToWorld.columns[2].z);
}

[[nodiscard]] math::Vec3 signed_extent(const math::Vec3 &direction,
                                       const math::Vec3 &extent) noexcept {
  return math::Vec3(direction.x >= 0.0F ? extent.x : -extent.x,
                    direction.y >= 0.0F ? extent.y : -extent.y,
                    direction.z >= 0.0F ? extent.z : -extent.z);
}

[[nodiscard]] math::Vec3
local_support_point(const ColliderWorldGeometry &geometry,
                    const math::Vec3 &localDirection) noexcept {
  switch (geometry.shape) {
  case math::ColliderShape::AABB:
  case math::ColliderShape::Heightfield:
    return signed_extent(localDirection, geometry.halfExtents);
  case math::ColliderShape::Sphere: {
    const float lengthSquared = math::dot(localDirection, localDirection);
    if (!(lengthSquared > 0.0F) || !finite(lengthSquared)) {
      return math::Vec3(0.0F, 0.0F, 0.0F);
    }
    return math::mul(localDirection,
                     geometry.halfExtents.x / std::sqrt(lengthSquared));
  }
  case math::ColliderShape::Capsule: {
    math::Vec3 point(0.0F,
                     localDirection.y >= 0.0F ? geometry.halfExtents.y
                                              : -geometry.halfExtents.y,
                     0.0F);
    const float lengthSquared = math::dot(localDirection, localDirection);
    if (lengthSquared > 0.0F && finite(lengthSquared)) {
      point = math::add(
          point, math::mul(localDirection,
                           geometry.halfExtents.x / std::sqrt(lengthSquared)));
    }
    return point;
  }
  case math::ColliderShape::ConvexHull: {
    const ConvexHullData *const hull = geometry.convexHull;
    if (hull == nullptr || hull->vertexCount == 0U) {
      return math::Vec3(0.0F, 0.0F, 0.0F);
    }
    std::size_t bestIndex = 0U;
    float bestProjection = math::dot(hull->vertices[0], localDirection);
    for (std::size_t index = 1U; index < hull->vertexCount; ++index) {
      const float projection = math::dot(hull->vertices[index], localDirection);
      if (projection > bestProjection) {
        bestProjection = projection;
        bestIndex = index;
      }
    }
    return hull->vertices[bestIndex];
  }
  }
  return math::Vec3(0.0F, 0.0F, 0.0F);
}

[[nodiscard]] bool valid_shape(const math::ColliderShape shape) noexcept {
  switch (shape) {
  case math::ColliderShape::AABB:
  case math::ColliderShape::Sphere:
  case math::ColliderShape::Capsule:
  case math::ColliderShape::ConvexHull:
  case math::ColliderShape::Heightfield:
    return true;
  }
  return false;
}

} // namespace

bool make_collider_world_geometry(
    const math::Collider &collider, const math::Mat4 &entityWorldMatrix,
    const ConvexHullData *const convexHull,
    ColliderWorldGeometry *const outGeometry) noexcept {
  if (outGeometry == nullptr || !finite(entityWorldMatrix) ||
      !affine(entityWorldMatrix) || !finite(collider.localPosition) ||
      !finite(collider.localRotation) || !finite(collider.halfExtents) ||
      !valid_shape(collider.shape)) {
    return false;
  }
  constexpr float minimumRotationLengthSquared = 1.0e-12F;
  const float rotationLengthSquared =
      math::dot(collider.localRotation, collider.localRotation);
  if (!finite(rotationLengthSquared) ||
      rotationLengthSquared <= minimumRotationLengthSquared) {
    return false;
  }

  if (collider.shape == math::ColliderShape::ConvexHull) {
    if (convexHull == nullptr || convexHull->vertexCount == 0U ||
        convexHull->vertexCount > ConvexHullData::kMaxVertices) {
      return false;
    }
    for (std::size_t index = 0U; index < convexHull->vertexCount; ++index) {
      if (!finite(convexHull->vertices[index])) {
        return false;
      }
    }
  }

  ColliderWorldGeometry geometry{};
  geometry.localToWorld =
      math::mul(entityWorldMatrix,
                math::compose_trs(collider.localPosition,
                                  math::normalize(collider.localRotation),
                                  math::Vec3(1.0F, 1.0F, 1.0F)));
  if (!finite(geometry.localToWorld) || !affine(geometry.localToWorld) ||
      !math::inverse(geometry.localToWorld, &geometry.worldToLocal) ||
      !finite(geometry.worldToLocal)) {
    return false;
  }

  geometry.center =
      transform_point(geometry.localToWorld, math::Vec3(0.0F, 0.0F, 0.0F));
  geometry.shape = collider.shape;
  geometry.halfExtents = math::Vec3(std::fabs(collider.halfExtents.x),
                                    std::fabs(collider.halfExtents.y),
                                    std::fabs(collider.halfExtents.z));
  geometry.convexHull = convexHull;

  const math::Vec3 positiveX =
      collider_support_point(geometry, math::Vec3(1.0F, 0.0F, 0.0F));
  const math::Vec3 negativeX =
      collider_support_point(geometry, math::Vec3(-1.0F, 0.0F, 0.0F));
  const math::Vec3 positiveY =
      collider_support_point(geometry, math::Vec3(0.0F, 1.0F, 0.0F));
  const math::Vec3 negativeY =
      collider_support_point(geometry, math::Vec3(0.0F, -1.0F, 0.0F));
  const math::Vec3 positiveZ =
      collider_support_point(geometry, math::Vec3(0.0F, 0.0F, 1.0F));
  const math::Vec3 negativeZ =
      collider_support_point(geometry, math::Vec3(0.0F, 0.0F, -1.0F));
  geometry.worldAabb = {math::Vec3(negativeX.x, negativeY.y, negativeZ.z),
                        math::Vec3(positiveX.x, positiveY.y, positiveZ.z)};

  if (!finite(geometry.center) || !finite(geometry.worldAabb.min) ||
      !finite(geometry.worldAabb.max)) {
    return false;
  }
  *outGeometry = geometry;
  return true;
}

math::Vec3 collider_support_point(const ColliderWorldGeometry &geometry,
                                  const math::Vec3 &worldDirection) noexcept {
  if (!finite(worldDirection) ||
      !(math::dot(worldDirection, worldDirection) > 0.0F)) {
    return geometry.center;
  }
  const math::Vec3 localDirection =
      support_direction_local(geometry.localToWorld, worldDirection);
  return transform_point(geometry.localToWorld,
                         local_support_point(geometry, localDirection));
}

} // namespace engine::physics
