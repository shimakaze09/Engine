#include "engine/physics/physics_query.h"

#include "engine/math/aabb.h"
#include "engine/math/sphere.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

namespace {

// Sphere vs AABB overlap test.
bool sphere_aabb_overlap(const math::Vec3 &center, float radius,
                         const math::AABB &box) noexcept {
  // Clamp sphere center to box, compute squared distance.
  float dx = center.x < box.min.x ? (box.min.x - center.x)
             : center.x > box.max.x ? (center.x - box.max.x)
                                     : 0.0F;
  float dy = center.y < box.min.y ? (box.min.y - center.y)
             : center.y > box.max.y ? (center.y - box.max.y)
                                     : 0.0F;
  float dz = center.z < box.min.z ? (box.min.z - center.z)
             : center.z > box.max.z ? (center.z - box.max.z)
                                     : 0.0F;
  return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
}

// Sphere vs sphere overlap test.
bool sphere_sphere_overlap(const math::Vec3 &a, float ra,
                           const math::Vec3 &b, float rb) noexcept {
  const math::Vec3 d = math::sub(a, b);
  const float rSum = ra + rb;
  return math::dot(d, d) <= (rSum * rSum);
}

// Check if collision mask includes the entity's layer.
bool passes_mask(const runtime::Collider &col,
                 std::uint32_t mask) noexcept {
  return (col.collisionLayer & mask) != 0U;
}

// AABB of a collider at a given transform position (axis-aligned only).
math::AABB collider_aabb(const runtime::Transform &t,
                          const runtime::Collider &col) noexcept {
  return math::aabb_from_center_half_extents(t.position, col.halfExtents);
}

// Swept sphere vs AABB: return time of impact in [0, maxT].
bool swept_sphere_aabb(const math::Vec3 &origin, float radius,
                       const math::Vec3 &dir, float maxT,
                       const math::AABB &box, float &outT) noexcept {
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
bool swept_box_aabb(const math::Vec3 &center,
                    const math::Vec3 &halfExtents,
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
  const float nx =
      halfExtents.x > eps ? (local.x / halfExtents.x) : 0.0F;
  const float ny =
      halfExtents.y > eps ? (local.y / halfExtents.y) : 0.0F;
  const float nz =
      halfExtents.z > eps ? (local.z / halfExtents.z) : 0.0F;

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

} // namespace

// ---------- raycast_all with mask -------------------------------------------

std::size_t raycast_all(const runtime::World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        runtime::PhysicsRaycastHit *outHits,
                        std::size_t maxHits,
                        std::uint32_t mask) noexcept {
  if (math::length_sq(direction) < 1e-12F) {
    return 0U;
  }
  if ((outHits == nullptr) || (maxHits == 0U)) {
    return 0U;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return 0U;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return 0U;
  }

  const math::Ray ray{origin, direction};
  std::size_t hitCount = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    runtime::Transform transform{};
    if (!world.get_transform(entities[i], &transform)) {
      continue;
    }

    float t = 0.0F;

    if (col.shape == runtime::ColliderShape::Sphere) {
      const math::Sphere sphere{transform.position, col.halfExtents.x};
      if (!math::ray_intersects_sphere(ray, sphere, &t)) {
        continue;
      }
    } else {
      const math::AABB box =
          math::aabb_from_center_half_extents(transform.position,
                                               col.halfExtents);
      if (!math::ray_intersects_aabb(ray, box, &t)) {
        continue;
      }
    }

    if ((t >= 0.0F) && (t <= maxDistance)) {
      if (hitCount < maxHits) {
        runtime::PhysicsRaycastHit &rh = outHits[hitCount];
        rh.entity = entities[i];
        rh.distance = t;
        rh.point = math::add(origin, math::mul(direction, t));
        if (col.shape == runtime::ColliderShape::Sphere) {
          rh.normal =
              math::normalize(math::sub(rh.point, transform.position));
        } else {
          rh.normal = aabb_hit_normal(rh.point, transform.position,
                                      col.halfExtents);
        }
        ++hitCount;
      }
    }
  }

  std::sort(outHits, outHits + hitCount,
            [](const runtime::PhysicsRaycastHit &a,
               const runtime::PhysicsRaycastHit &b) noexcept {
              return a.distance < b.distance;
            });

  return hitCount;
}

// ---------- overlap_sphere ---------------------------------------------------

std::size_t overlap_sphere(const runtime::World &world,
                           const math::Vec3 &center, float radius,
                           std::uint32_t *outEntityIndices,
                           std::size_t maxResults,
                           std::uint32_t mask) noexcept {
  if ((outEntityIndices == nullptr) || (maxResults == 0U)) {
    return 0U;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return 0U;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return 0U;
  }

  std::size_t resultCount = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    runtime::Transform t{};
    if (!world.get_transform(entities[i], &t)) {
      continue;
    }

    bool overlaps = false;
    if (col.shape == runtime::ColliderShape::Sphere) {
      overlaps = sphere_sphere_overlap(center, radius, t.position,
                                       col.halfExtents.x);
    } else {
      const math::AABB box = collider_aabb(t, col);
      overlaps = sphere_aabb_overlap(center, radius, box);
    }

    if (overlaps) {
      if (resultCount < maxResults) {
        outEntityIndices[resultCount] = entities[i].index;
      }
      ++resultCount;
    }
  }

  return resultCount < maxResults ? resultCount : maxResults;
}

// ---------- overlap_box ------------------------------------------------------

std::size_t overlap_box(const runtime::World &world, const math::Vec3 &center,
                        const math::Vec3 &halfExtents,
                        std::uint32_t *outEntityIndices,
                        std::size_t maxResults,
                        std::uint32_t mask) noexcept {
  if ((outEntityIndices == nullptr) || (maxResults == 0U)) {
    return 0U;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return 0U;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return 0U;
  }

  const math::AABB queryBox =
      math::aabb_from_center_half_extents(center, halfExtents);

  std::size_t resultCount = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    runtime::Transform t{};
    if (!world.get_transform(entities[i], &t)) {
      continue;
    }

    bool overlaps = false;
    if (col.shape == runtime::ColliderShape::Sphere) {
      // Sphere vs AABB.
      overlaps =
          sphere_aabb_overlap(t.position, col.halfExtents.x, queryBox);
    } else {
      const math::AABB box = collider_aabb(t, col);
      overlaps = math::aabb_intersects(queryBox, box);
    }

    if (overlaps) {
      if (resultCount < maxResults) {
        outEntityIndices[resultCount] = entities[i].index;
      }
      ++resultCount;
    }
  }

  return resultCount < maxResults ? resultCount : maxResults;
}

// ---------- sweep_sphere -----------------------------------------------------

bool sweep_sphere(const runtime::World &world, const math::Vec3 &origin,
                  float radius, const math::Vec3 &direction, float maxDistance,
                  SweepHit *outHit, std::uint32_t mask) noexcept {
  if (math::length_sq(direction) < 1e-12F) {
    return false;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return false;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }

  bool found = false;
  float bestT = maxDistance;

  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    runtime::Transform t{};
    if (!world.get_transform(entities[i], &t)) {
      continue;
    }

    float hitT = 0.0F;
    const math::AABB targetBox = collider_aabb(t, col);

    if (!swept_sphere_aabb(origin, radius, direction, bestT, targetBox,
                           hitT)) {
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
            math::add(origin, math::mul(direction, hitT));
        outHit->normal = aabb_hit_normal(outHit->contactPoint,
                                         t.position, col.halfExtents);
      }
    }
  }

  return found;
}

// ---------- sweep_box --------------------------------------------------------

bool sweep_box(const runtime::World &world, const math::Vec3 &center,
               const math::Vec3 &halfExtents, const math::Vec3 &direction,
               float maxDistance, SweepHit *outHit,
               std::uint32_t mask) noexcept {
  if (math::length_sq(direction) < 1e-12F) {
    return false;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return false;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }

  bool found = false;
  float bestT = maxDistance;

  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Collider &col = colliders[i];
    if (!passes_mask(col, mask)) {
      continue;
    }

    runtime::Transform t{};
    if (!world.get_transform(entities[i], &t)) {
      continue;
    }

    float hitT = 0.0F;
    const math::AABB targetBox = collider_aabb(t, col);

    if (!swept_box_aabb(center, halfExtents, direction, bestT, targetBox,
                        hitT)) {
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
            math::add(center, math::mul(direction, hitT));
        outHit->normal = aabb_hit_normal(outHit->contactPoint,
                                         t.position, col.halfExtents);
      }
    }
  }

  return found;
}

} // namespace engine::physics
