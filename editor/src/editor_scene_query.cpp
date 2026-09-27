// Implements the Scene view's queries declared in editor_scene_query.h.

#include "editor_scene_query.h"

#include <array>
#include <cmath>

#include "engine/math/aabb.h"
#include "engine/math/mat4.h"
#include "engine/physics/collider.h"
#include "engine/runtime/physics_bridge.h"

namespace engine::editor {

namespace {

/// Collider hits one pick considers; nearer ones win, so a crowd beyond
/// this many behind the cursor only drops the farthest.
constexpr std::size_t kMaxColliderHits = 64U;

bool box_contains(const math::Vec3 &center, const math::Vec3 &half,
                  const math::Vec3 &point) noexcept {
  return (std::fabs(point.x - center.x) <= half.x) &&
         (std::fabs(point.y - center.y) <= half.y) &&
         (std::fabs(point.z - center.z) <= half.z);
}

/// Records `entity` at `distance` in the nearest-first list `out` of
/// `*count` hits: an entity already listed keeps its nearer hit, and a
/// full list drops its farthest.
void record_hit(runtime::Entity entity, float distance, PickHit *out,
                std::size_t capacity, std::size_t *count) noexcept {
  for (std::size_t i = 0U; i < *count; ++i) {
    if (out[i].entity == entity) {
      if (distance >= out[i].distance) {
        return;
      }
      // Remove it; it is re-inserted at its nearer place below.
      for (std::size_t j = i + 1U; j < *count; ++j) {
        out[j - 1U] = out[j];
      }
      --*count;
      break;
    }
  }
  std::size_t at = *count;
  while ((at > 0U) && (out[at - 1U].distance > distance)) {
    --at;
  }
  if (at >= capacity) {
    return;
  }
  const std::size_t last = (*count < capacity) ? *count : (capacity - 1U);
  for (std::size_t j = last; j > at; --j) {
    out[j] = out[j - 1U];
  }
  out[at] = PickHit{entity, distance};
  if (*count < capacity) {
    ++*count;
  }
}

} // namespace

bool entity_mesh_world_box(const runtime::World &world, runtime::Entity entity,
                           MeshBoundsFn meshBounds, math::Vec3 *outCenter,
                           math::Vec3 *outHalf) noexcept {
  const runtime::MeshComponent *mesh = world.get_mesh_component_ptr(entity);
  const runtime::WorldTransform *worldTransform =
      world.get_world_transform_read_ptr(entity);
  math::Vec3 localCenter{};
  math::Vec3 localHalf{};
  if ((mesh == nullptr) || (worldTransform == nullptr) ||
      (meshBounds == nullptr) ||
      !meshBounds(mesh->meshAssetId, &localCenter, &localHalf)) {
    return false;
  }
  math::transform_aabb(worldTransform->matrix, localCenter, localHalf,
                       outCenter, outHalf);
  return true;
}

bool entity_collider_world_box(const runtime::World &world,
                               runtime::Entity entity, math::Vec3 *outCenter,
                               math::Vec3 *outHalf) noexcept {
  const runtime::Collider *collider = world.get_collider_ptr(entity);
  const runtime::WorldTransform *worldTransform =
      world.get_world_transform_read_ptr(entity);
  if ((collider == nullptr) || (worldTransform == nullptr)) {
    return false;
  }
  const physics::ConvexHullData *hull =
      (collider->shape == runtime::ColliderShape::ConvexHull)
          ? runtime::get_convex_hull_data(world, entity)
          : nullptr;
  physics::ColliderWorldGeometry geometry{};
  if (!physics::make_collider_world_geometry(*collider, worldTransform->matrix,
                                             hull, &geometry)) {
    return false;
  }
  *outCenter = math::aabb_center(geometry.worldAabb);
  *outHalf = math::aabb_half_extents(geometry.worldAabb);
  return true;
}

std::size_t scene_pick_hits(runtime::World &world, const math::Ray &ray,
                            float maxDistance, MeshBoundsFn meshBounds,
                            PickHit *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U) || !(maxDistance > 0.0F)) {
    return 0U;
  }
  std::size_t count = 0U;
  world.for_each_alive([&](runtime::Entity entity) noexcept {
    math::Vec3 center{};
    math::Vec3 half{};
    if (!entity_mesh_world_box(world, entity, meshBounds, &center, &half) ||
        box_contains(center, half, ray.origin)) {
      return;
    }
    const math::AABB box{math::sub(center, half), math::add(center, half)};
    float t = 0.0F;
    if (math::ray_intersects_aabb(ray, box, &t) && (t <= maxDistance)) {
      record_hit(entity, t, out, capacity, &count);
    }
  });

  std::array<physics::PhysicsRaycastHit, kMaxColliderHits> colliderHits{};
  const std::size_t colliderCount =
      runtime::raycast_all(world, ray.origin, ray.direction, maxDistance,
                           colliderHits.data(), colliderHits.size());
  for (std::size_t i = 0U; i < colliderCount; ++i) {
    // A ray that starts inside a shape hits it at distance zero.
    if (colliderHits[i].distance > 0.0F) {
      record_hit(colliderHits[i].entity, colliderHits[i].distance, out,
                 capacity, &count);
    }
  }
  return count;
}

runtime::Entity choose_pick(const PickHit *hits, std::size_t count,
                            runtime::Entity current,
                            bool sameSpotAsLastClick) noexcept {
  if ((hits == nullptr) || (count == 0U)) {
    return runtime::kInvalidEntity;
  }
  if (sameSpotAsLastClick) {
    for (std::size_t i = 0U; i < count; ++i) {
      if (hits[i].entity == current) {
        return hits[(i + 1U) % count].entity;
      }
    }
  }
  return hits[0].entity;
}

} // namespace engine::editor
