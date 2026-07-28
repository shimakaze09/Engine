// Implements physics query behavior for the Engine physics system.

#include "engine/physics/physics_query.h"

#include "engine/math/aabb.h"
#include "engine/math/mat4.h"
#include "engine/math/ray.h"
#include "engine/math/sphere.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/physics/collider.h"
#include "engine/physics/physics.h"

#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

namespace {

// Check if collision mask includes the entity's layer.
bool passes_mask(const Collider &col, std::uint32_t mask) noexcept {
  return (col.collisionLayer & mask) != 0U;
}

/// Transforms a point by an affine matrix.
math::Vec3 transform_point(const math::Mat4 &matrix,
                           const math::Vec3 &point) noexcept {
  const math::Vec4 result =
      math::mul(matrix, math::Vec4(point.x, point.y, point.z, 1.0F));
  return math::Vec3(result.x, result.y, result.z);
}

/// Transforms a vector by an affine matrix without applying translation.
math::Vec3 transform_vector(const math::Mat4 &matrix,
                            const math::Vec3 &vector) noexcept {
  const math::Vec4 result =
      math::mul(matrix, math::Vec4(vector.x, vector.y, vector.z, 0.0F));
  return math::Vec3(result.x, result.y, result.z);
}

/// Maps a local-space surface normal through an affine collider transform.
math::Vec3 transform_normal(const ColliderWorldGeometry &geometry,
                            const math::Vec3 &localNormal) noexcept {
  const math::Vec4 result =
      math::mul(math::transpose(geometry.worldToLocal),
                math::Vec4(localNormal.x, localNormal.y, localNormal.z, 0.0F));
  return math::normalize(math::Vec3(result.x, result.y, result.z));
}

/// Fetches one collider's authoritative parent-aware world geometry.
bool collider_geometry(const PhysicsWorldView &world, Entity entity,
                       const Collider &collider,
                       ColliderWorldGeometry *outGeometry) noexcept {
  PhysicsTransform worldTransform{};
  if (!world.get_physics_transform(entity, &worldTransform)) {
    return false;
  }
  const ConvexHullData *hull = nullptr;
  if (collider.shape == ColliderShape::ConvexHull) {
    hull = get_hull_data_ptr(world.physics_context(), entity);
  }
  return make_collider_world_geometry(collider, worldTransform.matrix, hull,
                                      outGeometry);
}

// Swept sphere vs AABB: return time of impact in [0, maxT].
bool swept_sphere_aabb(const math::Vec3 &origin, float radius,
                       const math::Vec3 &dir, float maxT, const math::AABB &box,
                       float &outT) noexcept {
  // Expand the AABB by the sphere radius, then raycast.
  math::AABB expanded{};
  expanded.min = math::sub(box.min, math::Vec3(radius, radius, radius));
  expanded.max = math::add(box.max, math::Vec3(radius, radius, radius));

  const math::Ray ray{origin, dir};
  float t = 0.0F;
  if (!math::ray_intersects_aabb(ray, expanded, &t)) {
    return false;
  }
  if ((t < 0.0F) || (t > maxT)) {
    return false;
  }
  outT = t;
  return true;
}

// Swept box vs AABB via Minkowski expansion.
bool swept_box_aabb(const math::Vec3 &center, const math::Vec3 &halfExtents,
                    const math::Vec3 &dir, float maxT,
                    const math::AABB &targetBox, float &outT) noexcept {
  // Expand target by moving box half extents.
  math::AABB expanded{};
  expanded.min = math::sub(targetBox.min, halfExtents);
  expanded.max = math::add(targetBox.max, halfExtents);

  const math::Ray ray{center, dir};
  float t = 0.0F;
  if (!math::ray_intersects_aabb(ray, expanded, &t)) {
    return false;
  }
  if ((t < 0.0F) || (t > maxT)) {
    return false;
  }
  outT = t;
  return true;
}

// Compute AABB hit normal (closest face).
math::Vec3 aabb_hit_normal(const math::Vec3 &hitPoint,
                           const math::Vec3 &boxCenter,
                           const math::Vec3 &halfExtents) noexcept {
  const math::Vec3 local = math::sub(hitPoint, boxCenter);
  const float eps = 1e-4F;
  const float nx = halfExtents.x > eps ? (local.x / halfExtents.x) : 0.0F;
  const float ny = halfExtents.y > eps ? (local.y / halfExtents.y) : 0.0F;
  const float nz = halfExtents.z > eps ? (local.z / halfExtents.z) : 0.0F;

  const float ax = std::fabs(nx);
  const float ay = std::fabs(ny);
  const float az = std::fabs(nz);

  if (ax >= ay && ax >= az) {
    return math::Vec3(nx > 0.0F ? 1.0F : -1.0F, 0.0F, 0.0F);
  }
  if (ay >= az) {
    return math::Vec3(0.0F, ny > 0.0F ? 1.0F : -1.0F, 0.0F);
  }
  return math::Vec3(0.0F, 0.0F, nz > 0.0F ? 1.0F : -1.0F);
}

/// Intersects a local-space Y-axis capsule with an arbitrary-parameter ray.
bool ray_intersects_capsule(const math::Ray &ray, float halfHeight,
                            float radius, float maxDistance, float *outT,
                            math::Vec3 *outNormal) noexcept {
  float bestT = maxDistance + 1.0F;
  math::Vec3 bestNormal(0.0F, 1.0F, 0.0F);

  const float cylinderA =
      ray.direction.x * ray.direction.x + ray.direction.z * ray.direction.z;
  const float cylinderB =
      2.0F * (ray.origin.x * ray.direction.x + ray.origin.z * ray.direction.z);
  const float cylinderC = ray.origin.x * ray.origin.x +
                          ray.origin.z * ray.origin.z - radius * radius;
  const float cylinderDiscriminant =
      cylinderB * cylinderB - 4.0F * cylinderA * cylinderC;
  if ((cylinderA > 1.0e-12F) && (cylinderDiscriminant >= 0.0F)) {
    const float root = std::sqrt(cylinderDiscriminant);
    const float inverseDenominator = 1.0F / (2.0F * cylinderA);
    const float candidates[2] = {(-cylinderB - root) * inverseDenominator,
                                 (-cylinderB + root) * inverseDenominator};
    for (float candidate : candidates) {
      const float hitY = ray.origin.y + ray.direction.y * candidate;
      if ((candidate >= 0.0F) && (candidate < bestT) && (hitY >= -halfHeight) &&
          (hitY <= halfHeight)) {
        bestT = candidate;
        bestNormal = math::normalize(
            math::Vec3(ray.origin.x + ray.direction.x * candidate, 0.0F,
                       ray.origin.z + ray.direction.z * candidate));
      }
    }
  }

  for (int hemisphere = -1; hemisphere <= 1; hemisphere += 2) {
    const math::Vec3 center(0.0F, static_cast<float>(hemisphere) * halfHeight,
                            0.0F);
    float candidate = 0.0F;
    if (!math::ray_intersects_sphere(ray, math::Sphere{center, radius},
                                     &candidate) ||
        (candidate < 0.0F) || (candidate >= bestT)) {
      continue;
    }
    const math::Vec3 point =
        math::add(ray.origin, math::mul(ray.direction, candidate));
    const float relativeY = point.y - center.y;
    if (((hemisphere > 0) && (relativeY >= 0.0F)) ||
        ((hemisphere < 0) && (relativeY <= 0.0F))) {
      bestT = candidate;
      bestNormal = math::normalize(math::sub(point, center));
    }
  }

  if (bestT > maxDistance) {
    return false;
  }
  if (outT != nullptr) {
    *outT = bestT;
  }
  if (outNormal != nullptr) {
    *outNormal = bestNormal;
  }
  return true;
}

/// Intersects a local-space convex hull using its outward face planes.
bool ray_intersects_convex_hull(const math::Ray &ray,
                                const ConvexHullData &hull, float maxDistance,
                                float *outT, math::Vec3 *outNormal) noexcept {
  float nearT = 0.0F;
  float farT = maxDistance;
  math::Vec3 nearNormal(0.0F, 1.0F, 0.0F);
  for (std::size_t index = 0U; index < hull.planeCount; ++index) {
    const ConvexHullData::Plane &plane = hull.planes[index];
    const float denominator = math::dot(plane.normal, ray.direction);
    const float numerator =
        plane.distance - math::dot(plane.normal, ray.origin);
    if (std::fabs(denominator) < 1.0e-10F) {
      if (numerator < 0.0F) {
        return false;
      }
      continue;
    }
    const float candidate = numerator / denominator;
    if (denominator < 0.0F) {
      if (candidate > nearT) {
        nearT = candidate;
        nearNormal = plane.normal;
      }
    } else if (candidate < farT) {
      farT = candidate;
    }
    if (nearT > farT) {
      return false;
    }
  }
  if ((nearT < 0.0F) || (nearT > maxDistance)) {
    return false;
  }
  if (outT != nullptr) {
    *outT = nearT;
  }
  if (outNormal != nullptr) {
    *outNormal = nearNormal;
  }
  return true;
}

/// Tests a ray against one heightfield triangle without back-face culling.
bool ray_intersects_triangle(const math::Ray &ray, const math::Vec3 &a,
                             const math::Vec3 &b, const math::Vec3 &c,
                             float maxDistance, float *outT,
                             math::Vec3 *outNormal) noexcept {
  const math::Vec3 edgeAB = math::sub(b, a);
  const math::Vec3 edgeAC = math::sub(c, a);
  const math::Vec3 crossDirection = math::cross(ray.direction, edgeAC);
  const float determinant = math::dot(edgeAB, crossDirection);
  if (std::fabs(determinant) <= 1.0e-10F) {
    return false;
  }
  const float inverseDeterminant = 1.0F / determinant;
  const math::Vec3 originOffset = math::sub(ray.origin, a);
  const float u = math::dot(originOffset, crossDirection) * inverseDeterminant;
  if ((u < 0.0F) || (u > 1.0F)) {
    return false;
  }
  const math::Vec3 crossOffset = math::cross(originOffset, edgeAB);
  const float v = math::dot(ray.direction, crossOffset) * inverseDeterminant;
  if ((v < 0.0F) || ((u + v) > 1.0F)) {
    return false;
  }
  const float candidate = math::dot(edgeAC, crossOffset) * inverseDeterminant;
  if ((candidate < 0.0F) || (candidate > maxDistance)) {
    return false;
  }
  if (outT != nullptr) {
    *outT = candidate;
  }
  if (outNormal != nullptr) {
    math::Vec3 normal = math::normalize(math::cross(edgeAB, edgeAC));
    if (normal.y < 0.0F) {
      normal = math::mul(normal, -1.0F);
    }
    *outNormal = normal;
  }
  return true;
}

/// Intersects all fixed-capacity heightfield triangles in local space.
bool ray_intersects_heightfield(const math::Ray &ray,
                                const HeightfieldData &heightfield,
                                float maxDistance, float *outT,
                                math::Vec3 *outNormal) noexcept {
  if ((heightfield.rows < 2U) || (heightfield.columns < 2U) ||
      !(heightfield.spacingX > 0.0F) || !(heightfield.spacingZ > 0.0F)) {
    return false;
  }
  const float width =
      static_cast<float>(heightfield.columns - 1U) * heightfield.spacingX;
  const float depth =
      static_cast<float>(heightfield.rows - 1U) * heightfield.spacingZ;
  float bestT = maxDistance + 1.0F;
  math::Vec3 bestNormal(0.0F, 1.0F, 0.0F);
  for (std::size_t row = 0U; row + 1U < heightfield.rows; ++row) {
    for (std::size_t column = 0U; column + 1U < heightfield.columns; ++column) {
      const auto vertex = [&](std::size_t x, std::size_t z) noexcept {
        return math::Vec3(
            -width * 0.5F + static_cast<float>(x) * heightfield.spacingX,
            heightfield.heights[z * heightfield.columns + x],
            -depth * 0.5F + static_cast<float>(z) * heightfield.spacingZ);
      };
      const math::Vec3 v00 = vertex(column, row);
      const math::Vec3 v10 = vertex(column + 1U, row);
      const math::Vec3 v01 = vertex(column, row + 1U);
      const math::Vec3 v11 = vertex(column + 1U, row + 1U);
      float candidate = 0.0F;
      math::Vec3 candidateNormal{};
      if (ray_intersects_triangle(ray, v00, v01, v10, bestT, &candidate,
                                  &candidateNormal) &&
          (candidate < bestT)) {
        bestT = candidate;
        bestNormal = candidateNormal;
      }
      if (ray_intersects_triangle(ray, v10, v01, v11, bestT, &candidate,
                                  &candidateNormal) &&
          (candidate < bestT)) {
        bestT = candidate;
        bestNormal = candidateNormal;
      }
    }
  }
  if (bestT > maxDistance) {
    return false;
  }
  if (outT != nullptr) {
    *outT = bestT;
  }
  if (outNormal != nullptr) {
    *outNormal = bestNormal;
  }
  return true;
}

/// Intersects a world ray with authoritative affine collider geometry.
bool ray_intersects_geometry(const PhysicsWorldView &world, Entity entity,
                             const ColliderWorldGeometry &geometry,
                             const math::Ray &worldRay, float maxDistance,
                             float *outT, math::Vec3 *outNormal) noexcept {
  const math::Ray localRay{
      transform_point(geometry.worldToLocal, worldRay.origin),
      transform_vector(geometry.worldToLocal, worldRay.direction)};
  float hitT = 0.0F;
  math::Vec3 localNormal(0.0F, 1.0F, 0.0F);
  bool hit = false;
  if (geometry.shape == ColliderShape::Sphere) {
    hit = math::ray_intersects_sphere(
        localRay, math::Sphere{{}, geometry.halfExtents.x}, &hitT);
    if (hit) {
      const math::Vec3 localPoint =
          math::add(localRay.origin, math::mul(localRay.direction, hitT));
      localNormal = math::normalize(localPoint);
    }
  } else if (geometry.shape == ColliderShape::Capsule) {
    hit = ray_intersects_capsule(localRay, geometry.halfExtents.y,
                                 geometry.halfExtents.x, maxDistance, &hitT,
                                 &localNormal);
  } else if (geometry.shape == ColliderShape::ConvexHull) {
    hit = geometry.convexHull != nullptr &&
          ray_intersects_convex_hull(localRay, *geometry.convexHull,
                                     maxDistance, &hitT, &localNormal);
  } else if (geometry.shape == ColliderShape::Heightfield) {
    const HeightfieldData *heightfield =
        get_heightfield_data(world.physics_context(), entity);
    hit = heightfield != nullptr &&
          ray_intersects_heightfield(localRay, *heightfield, maxDistance, &hitT,
                                     &localNormal);
  } else {
    const math::AABB localBox =
        math::aabb_from_center_half_extents(math::Vec3(), geometry.halfExtents);
    hit = math::ray_intersects_aabb(localRay, localBox, &hitT);
    if (hit) {
      const math::Vec3 localPoint =
          math::add(localRay.origin, math::mul(localRay.direction, hitT));
      localNormal =
          aabb_hit_normal(localPoint, math::Vec3(), geometry.halfExtents);
    }
  }
  if (!hit || (hitT < 0.0F) || (hitT > maxDistance)) {
    return false;
  }
  if (outT != nullptr) {
    *outT = hitT;
  }
  if (outNormal != nullptr) {
    *outNormal = transform_normal(geometry, localNormal);
  }
  return true;
}

/// Fixed-size simplex used by the overlap-only GJK query path.
struct QuerySimplex final {
  math::Vec3 points[4]{};
  std::size_t count = 0U;

  /// Prepends the newest Minkowski support point.
  void push(const math::Vec3 &point) noexcept {
    for (std::size_t index = std::min(count, std::size_t{3U}); index > 0U;
         --index) {
      points[index] = points[index - 1U];
    }
    points[0] = point;
    count = std::min(count + 1U, std::size_t{4U});
  }
};

/// Returns a vector toward the origin perpendicular to one simplex edge.
math::Vec3 edge_search_direction(const math::Vec3 &edge,
                                 const math::Vec3 &towardOrigin) noexcept {
  return math::cross(math::cross(edge, towardOrigin), edge);
}

/// Reduces a line simplex to the feature closest to the origin.
bool reduce_line(QuerySimplex &simplex, math::Vec3 *direction) noexcept {
  const math::Vec3 a = simplex.points[0];
  const math::Vec3 b = simplex.points[1];
  const math::Vec3 ab = math::sub(b, a);
  const math::Vec3 ao = math::mul(a, -1.0F);
  if (math::dot(ab, ao) > 0.0F) {
    *direction = edge_search_direction(ab, ao);
    return math::length_sq(*direction) <= 1.0e-16F;
  }
  simplex.points[0] = a;
  simplex.count = 1U;
  *direction = ao;
  return math::length_sq(*direction) <= 1.0e-16F;
}

/// Reduces a triangle simplex or reports that its face contains the origin.
bool reduce_triangle(QuerySimplex &simplex, math::Vec3 *direction) noexcept {
  const math::Vec3 a = simplex.points[0];
  const math::Vec3 b = simplex.points[1];
  const math::Vec3 c = simplex.points[2];
  const math::Vec3 ab = math::sub(b, a);
  const math::Vec3 ac = math::sub(c, a);
  const math::Vec3 ao = math::mul(a, -1.0F);
  const math::Vec3 abc = math::cross(ab, ac);

  if (math::dot(math::cross(abc, ac), ao) > 0.0F) {
    if (math::dot(ac, ao) > 0.0F) {
      simplex.points[1] = c;
      simplex.count = 2U;
      *direction = edge_search_direction(ac, ao);
      return math::length_sq(*direction) <= 1.0e-16F;
    }
    simplex.points[1] = b;
    simplex.count = 2U;
    return reduce_line(simplex, direction);
  }
  if (math::dot(math::cross(ab, abc), ao) > 0.0F) {
    simplex.points[1] = b;
    simplex.count = 2U;
    return reduce_line(simplex, direction);
  }
  if (math::dot(abc, ao) > 0.0F) {
    *direction = abc;
  } else {
    simplex.points[1] = c;
    simplex.points[2] = b;
    *direction = math::mul(abc, -1.0F);
  }
  return math::length_sq(*direction) <= 1.0e-16F;
}

/// Reduces a tetrahedron simplex or reports that it contains the origin.
bool reduce_tetrahedron(QuerySimplex &simplex, math::Vec3 *direction) noexcept {
  const math::Vec3 a = simplex.points[0];
  const math::Vec3 b = simplex.points[1];
  const math::Vec3 c = simplex.points[2];
  const math::Vec3 d = simplex.points[3];
  const math::Vec3 ao = math::mul(a, -1.0F);
  const math::Vec3 ab = math::sub(b, a);
  const math::Vec3 ac = math::sub(c, a);
  const math::Vec3 ad = math::sub(d, a);
  const math::Vec3 abc = math::cross(ab, ac);
  const math::Vec3 acd = math::cross(ac, ad);
  const math::Vec3 adb = math::cross(ad, ab);

  if (math::dot(abc, ao) > 0.0F) {
    simplex.count = 3U;
    return reduce_triangle(simplex, direction);
  }
  if (math::dot(acd, ao) > 0.0F) {
    simplex.points[1] = c;
    simplex.points[2] = d;
    simplex.count = 3U;
    return reduce_triangle(simplex, direction);
  }
  if (math::dot(adb, ao) > 0.0F) {
    simplex.points[1] = d;
    simplex.points[2] = b;
    simplex.count = 3U;
    return reduce_triangle(simplex, direction);
  }
  return true;
}

/// Tests two convex world geometries without requesting EPA penetration data.
bool convex_geometries_overlap(const ColliderWorldGeometry &a,
                               const ColliderWorldGeometry &b) noexcept {
  math::Vec3 direction = math::sub(b.center, a.center);
  if (math::length_sq(direction) <= 1.0e-16F) {
    direction = math::Vec3(1.0F, 0.0F, 0.0F);
  }
  QuerySimplex simplex{};
  const auto support = [&](const math::Vec3 &axis) noexcept {
    return math::sub(collider_support_point(a, axis),
                     collider_support_point(b, math::mul(axis, -1.0F)));
  };
  simplex.push(support(direction));
  direction = math::mul(simplex.points[0], -1.0F);

  for (std::size_t iteration = 0U; iteration < 32U; ++iteration) {
    if (math::length_sq(direction) <= 1.0e-16F) {
      return true;
    }
    const math::Vec3 point = support(direction);
    if (math::dot(point, direction) < 0.0F) {
      return false;
    }
    simplex.push(point);
    if ((simplex.count == 2U && reduce_line(simplex, &direction)) ||
        (simplex.count == 3U && reduce_triangle(simplex, &direction)) ||
        (simplex.count == 4U && reduce_tetrahedron(simplex, &direction))) {
      return true;
    }
  }
  return false;
}

/// Builds a world-axis query collider with identity local pose.
bool query_geometry(ColliderShape shape, const math::Vec3 &center,
                    const math::Vec3 &halfExtents,
                    ColliderWorldGeometry *outGeometry) noexcept {
  Collider queryCollider{};
  queryCollider.shape = shape;
  queryCollider.halfExtents = halfExtents;
  return make_collider_world_geometry(
      queryCollider,
      math::compose_trs(center, math::Quat(), math::Vec3(1.0F, 1.0F, 1.0F)),
      nullptr, outGeometry);
}

/// Tests two convex affine geometries, using AABBs for heightfields only.
bool geometries_overlap(const ColliderWorldGeometry &query,
                        const ColliderWorldGeometry &target) noexcept {
  if (!math::aabb_intersects(query.worldAabb, target.worldAabb)) {
    return false;
  }
  if ((query.shape == ColliderShape::Heightfield) ||
      (target.shape == ColliderShape::Heightfield)) {
    return true;
  }
  return convex_geometries_overlap(query, target);
}

/// Normalizes a finite query direction and validates its world-space range.
bool normalize_query_direction(const math::Vec3 &direction, float maxDistance,
                               math::Vec3 *outDirection) noexcept {
  if ((outDirection == nullptr) || !std::isfinite(maxDistance) ||
      (maxDistance <= 0.0F)) {
    return false;
  }

  const float lengthSquared = math::length_sq(direction);
  if (!std::isfinite(lengthSquared) || (lengthSquared < 1.0e-12F)) {
    return false;
  }

  const float inverseLength = 1.0F / std::sqrt(lengthSquared);
  *outDirection = math::mul(direction, inverseLength);
  return true;
}

} // namespace

bool raycast(const PhysicsWorldView &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit, Entity skipEntity) noexcept {
  math::Vec3 normalizedDirection{};
  if (!normalize_query_direction(direction, maxDistance,
                                 &normalizedDirection)) {
    return false;
  }

  const std::size_t count = world.collider_count();
  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if ((count == 0U) ||
      !world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }

  const math::Ray ray{origin, normalizedDirection};
  PhysicsRaycastHit closest{};
  float closestDistance = maxDistance;
  bool found = false;
  for (std::size_t i = 0U; i < count; ++i) {
    if (entities[i] == skipEntity) {
      continue;
    }
    ColliderWorldGeometry geometry{};
    if (!collider_geometry(world, entities[i], colliders[i], &geometry)) {
      continue;
    }

    float distance = 0.0F;
    math::Vec3 normal{};
    if (!ray_intersects_geometry(world, entities[i], geometry, ray, maxDistance,
                                 &distance, &normal) ||
        (distance < 0.0F) || (distance > closestDistance)) {
      continue;
    }
    if (found && (distance == closestDistance) &&
        (entities[i].index >= closest.entity.index)) {
      continue;
    }

    found = true;
    closestDistance = distance;
    closest.entity = entities[i];
    closest.distance = distance;
    closest.point = math::add(origin, math::mul(normalizedDirection, distance));
    closest.normal = normal;
  }

  if (found && (outHit != nullptr)) {
    *outHit = closest;
  }
  return found;
}

// ---------- raycast_all with mask -------------------------------------------

std::size_t raycast_all(const PhysicsWorldView &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        PhysicsRaycastHit *outHits, std::size_t maxHits,
                        std::uint32_t mask) noexcept {
  if ((outHits == nullptr) || (maxHits == 0U)) {
    return 0U;
  }

  math::Vec3 normalizedDirection{};
  if (!normalize_query_direction(direction, maxDistance,
                                 &normalizedDirection)) {
    return 0U;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return 0U;
  }

  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return 0U;
  }

  const math::Ray ray{origin, normalizedDirection};
  std::size_t hitCount = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    const Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    ColliderWorldGeometry geometry{};
    if (!collider_geometry(world, entities[i], col, &geometry)) {
      continue;
    }

    float t = 0.0F;
    math::Vec3 normal{};
    if (!ray_intersects_geometry(world, entities[i], geometry, ray, maxDistance,
                                 &t, &normal)) {
      continue;
    }

    if ((t >= 0.0F) && (t <= maxDistance)) {
      std::size_t outputIndex = hitCount;
      if (hitCount == maxHits) {
        outputIndex = 0U;
        for (std::size_t hitIndex = 1U; hitIndex < hitCount; ++hitIndex) {
          if (outHits[hitIndex].distance > outHits[outputIndex].distance) {
            outputIndex = hitIndex;
          }
        }
        if (t >= outHits[outputIndex].distance) {
          continue;
        }
      } else {
        ++hitCount;
      }

      PhysicsRaycastHit &hit = outHits[outputIndex];
      hit.entity = entities[i];
      hit.distance = t;
      hit.point = math::add(origin, math::mul(normalizedDirection, t));
      hit.normal = normal;
    }
  }

  std::sort(
      outHits, outHits + hitCount,
      [](const PhysicsRaycastHit &a, const PhysicsRaycastHit &b) noexcept {
        if (a.distance != b.distance) {
          return a.distance < b.distance;
        }
        if (a.entity.index != b.entity.index) {
          return a.entity.index < b.entity.index;
        }
        return a.entity.generation < b.entity.generation;
      });

  return hitCount;
}

// ---------- overlap_sphere ---------------------------------------------------

std::size_t overlap_sphere(const PhysicsWorldView &world,
                           const math::Vec3 &center, float radius,
                           std::uint32_t *outEntityIndices,
                           std::size_t maxResults,
                           std::uint32_t mask) noexcept {
  if ((outEntityIndices == nullptr) || (maxResults == 0U)) {
    return 0U;
  }

  if (!std::isfinite(radius) || (radius < 0.0F)) {
    return 0U;
  }

  ColliderWorldGeometry query{};
  if (!query_geometry(ColliderShape::Sphere, center,
                      math::Vec3(radius, radius, radius), &query)) {
    return 0U;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return 0U;
  }

  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return 0U;
  }

  std::size_t resultCount = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    const Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    ColliderWorldGeometry target{};
    if (!collider_geometry(world, entities[i], col, &target)) {
      continue;
    }

    if (geometries_overlap(query, target)) {
      if (resultCount < maxResults) {
        outEntityIndices[resultCount] = entities[i].index;
      }
      ++resultCount;
    }
  }

  return resultCount < maxResults ? resultCount : maxResults;
}

// ---------- overlap_box ------------------------------------------------------

std::size_t overlap_box(const PhysicsWorldView &world, const math::Vec3 &center,
                        const math::Vec3 &halfExtents,
                        std::uint32_t *outEntityIndices, std::size_t maxResults,
                        std::uint32_t mask) noexcept {
  if ((outEntityIndices == nullptr) || (maxResults == 0U)) {
    return 0U;
  }

  if (!std::isfinite(halfExtents.x) || !std::isfinite(halfExtents.y) ||
      !std::isfinite(halfExtents.z) || (halfExtents.x < 0.0F) ||
      (halfExtents.y < 0.0F) || (halfExtents.z < 0.0F)) {
    return 0U;
  }

  ColliderWorldGeometry query{};
  if (!query_geometry(ColliderShape::AABB, center, halfExtents, &query)) {
    return 0U;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return 0U;
  }

  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return 0U;
  }

  std::size_t resultCount = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    const Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    ColliderWorldGeometry target{};
    if (!collider_geometry(world, entities[i], col, &target)) {
      continue;
    }

    if (geometries_overlap(query, target)) {
      if (resultCount < maxResults) {
        outEntityIndices[resultCount] = entities[i].index;
      }
      ++resultCount;
    }
  }

  return resultCount < maxResults ? resultCount : maxResults;
}

// ---------- sweep_sphere -----------------------------------------------------

bool sweep_sphere(const PhysicsWorldView &world, const math::Vec3 &origin,
                  float radius, const math::Vec3 &direction, float maxDistance,
                  SweepHit *outHit, std::uint32_t mask) noexcept {
  math::Vec3 normalizedDirection{};
  if (!std::isfinite(radius) || (radius < 0.0F) ||
      !normalize_query_direction(direction, maxDistance,
                                 &normalizedDirection)) {
    return false;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return false;
  }

  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }

  bool found = false;
  float bestT = maxDistance;

  for (std::size_t i = 0U; i < count; ++i) {
    const Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    ColliderWorldGeometry geometry{};
    if (!collider_geometry(world, entities[i], col, &geometry)) {
      continue;
    }

    float hitT = 0.0F;
    const math::AABB &targetBox = geometry.worldAabb;

    if (!swept_sphere_aabb(origin, radius, normalizedDirection, bestT,
                           targetBox, hitT)) {
      continue;
    }

    if (hitT <= bestT) {
      bestT = hitT;
      found = true;
      if (outHit != nullptr) {
        outHit->entityIndex = entities[i].index;
        outHit->timeOfImpact = hitT / maxDistance;
        outHit->distance = hitT;
        outHit->contactPoint =
            math::add(origin, math::mul(normalizedDirection, hitT));
        const math::Vec3 boxCenter =
            math::mul(math::add(targetBox.min, targetBox.max), 0.5F);
        const math::Vec3 boxHalfExtents =
            math::mul(math::sub(targetBox.max, targetBox.min), 0.5F);
        outHit->normal =
            aabb_hit_normal(outHit->contactPoint, boxCenter, boxHalfExtents);
      }
    }
  }

  return found;
}

// ---------- sweep_box --------------------------------------------------------

bool sweep_box(const PhysicsWorldView &world, const math::Vec3 &center,
               const math::Vec3 &halfExtents, const math::Vec3 &direction,
               float maxDistance, SweepHit *outHit,
               std::uint32_t mask) noexcept {
  math::Vec3 normalizedDirection{};
  if (!std::isfinite(halfExtents.x) || !std::isfinite(halfExtents.y) ||
      !std::isfinite(halfExtents.z) || (halfExtents.x < 0.0F) ||
      (halfExtents.y < 0.0F) || (halfExtents.z < 0.0F) ||
      !normalize_query_direction(direction, maxDistance,
                                 &normalizedDirection)) {
    return false;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return false;
  }

  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }

  bool found = false;
  float bestT = maxDistance;

  for (std::size_t i = 0U; i < count; ++i) {
    const Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    ColliderWorldGeometry geometry{};
    if (!collider_geometry(world, entities[i], col, &geometry)) {
      continue;
    }

    float hitT = 0.0F;
    const math::AABB &targetBox = geometry.worldAabb;

    if (!swept_box_aabb(center, halfExtents, normalizedDirection, bestT,
                        targetBox, hitT)) {
      continue;
    }

    if (hitT <= bestT) {
      bestT = hitT;
      found = true;
      if (outHit != nullptr) {
        outHit->entityIndex = entities[i].index;
        outHit->timeOfImpact = hitT / maxDistance;
        outHit->distance = hitT;
        outHit->contactPoint =
            math::add(center, math::mul(normalizedDirection, hitT));
        const math::Vec3 boxCenter =
            math::mul(math::add(targetBox.min, targetBox.max), 0.5F);
        const math::Vec3 boxHalfExtents =
            math::mul(math::sub(targetBox.max, targetBox.min), 0.5F);
        outHit->normal =
            aabb_hit_normal(outHit->contactPoint, boxCenter, boxHalfExtents);
      }
    }
  }

  return found;
}

} // namespace engine::physics
