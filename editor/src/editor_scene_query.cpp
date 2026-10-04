// Implements the Scene view's queries declared in editor_scene_query.h.

#include "editor_scene_query.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "engine/math/aabb.h"
#include "engine/math/mat4.h"
#include "engine/physics/collider.h"
#include "engine/runtime/camera_component_update.h"
#include "engine/runtime/light_pose.h"
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
  physics::ColliderWorldGeometry geometry{};
  if (!runtime::collider_world_geometry(world, entity, *collider,
                                        worldTransform->matrix, &geometry)) {
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

bool within_click_slop(float dx, float dy) noexcept {
  return ((dx * dx) + (dy * dy)) <= (kClickSlopPixels * kClickSlopPixels);
}

bool ray_ground_point(const math::Ray &ray, float reach,
                      math::Vec3 *out) noexcept {
  // A direction this flat meets the ground too far out to be meant.
  constexpr float kMinDescent = 1.0e-4F;
  if ((out == nullptr) || !(ray.direction.y < -kMinDescent)) {
    return false;
  }
  const float distance = -ray.origin.y / ray.direction.y;
  if (!(distance >= 0.0F) || !(distance <= reach)) {
    return false;
  }
  *out = math::Vec3(ray.origin.x + (ray.direction.x * distance), 0.0F,
                    ray.origin.z + (ray.direction.z * distance));
  return true;
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

namespace {

/// Where `entity`'s icon stands in the world; false without an icon.
bool icon_position(const runtime::World &world, runtime::Entity entity,
                   SceneIconKind *outKind, math::Vec3 *outPosition) noexcept {
  const bool light = world.has_light_component(entity) ||
                     world.has_point_light_component(entity) ||
                     world.has_spot_light_component(entity);
  if (!light && !world.has_camera_component(entity)) {
    return false;
  }
  *outKind = light ? SceneIconKind::Light : SceneIconKind::Camera;
  if (light) {
    *outPosition =
        runtime::light_world_pose(world, entity, math::Vec3()).position;
  } else {
    const runtime::WorldTransform *worldTransform =
        world.get_world_transform_read_ptr(entity);
    *outPosition =
        (worldTransform != nullptr) ? worldTransform->position : math::Vec3();
  }
  return true;
}

} // namespace

bool entity_has_icon(const runtime::World &world,
                     runtime::Entity entity) noexcept {
  SceneIconKind kind = SceneIconKind::Light;
  math::Vec3 position{};
  return icon_position(world, entity, &kind, &position);
}

bool game_camera_notice(const runtime::World &world, char *out,
                        std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  std::uint32_t tieCount = 0U;
  const runtime::Entity camera =
      runtime::find_authored_active_camera(world, &tieCount);
  int written = 0;
  if (camera == runtime::kInvalidEntity) {
    written = std::snprintf(out, capacity,
                            "No camera renders the game. Create one, or "
                            "make a Camera active.");
  } else if (tieCount > 0U) {
    runtime::NameComponent name{};
    const char *label =
        (world.get_name_component(camera, &name) && (name.name[0] != '\0'))
            ? name.name
            : "<unnamed>";
    written = std::snprintf(
        out, capacity,
        "Camera \"%.48s\" ties in priority with %u other%s: raise the "
        "priority of the one that should render",
        label, tieCount, (tieCount == 1U) ? "" : "s");
  } else {
    return false;
  }
  if ((written < 0) || (static_cast<std::size_t>(written) >= capacity)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

bool game_view_has_camera(const runtime::World &world) noexcept {
  return runtime::find_authored_active_camera(world, nullptr) !=
         runtime::kInvalidEntity;
}

SceneIconMetrics scene_icon_metrics(float uiScale, float iconScale) noexcept {
  const float ui =
      (std::isfinite(uiScale) && (uiScale > 0.0F)) ? uiScale : 1.0F;
  const float icon =
      (std::isfinite(iconScale) && (iconScale > 0.0F))
          ? std::clamp(iconScale, kMinSceneIconScale, kMaxSceneIconScale)
          : 1.0F;
  const float scale = ui * icon;
  SceneIconMetrics metrics{};
  metrics.radius = kSceneIconBasePixels * 0.5F * scale;
  metrics.stroke = 1.5F * scale;
  metrics.selectionRadius = metrics.radius + (2.0F * metrics.stroke);
  return metrics;
}

std::size_t scene_icons(const runtime::World &world,
                        const math::Mat4 &viewProjection, SceneIcon *out,
                        std::size_t capacity) noexcept {
  if (out == nullptr) {
    return 0U;
  }
  std::size_t count = 0U;
  world.for_each_alive([&](runtime::Entity entity) noexcept {
    SceneIcon icon{};
    math::Vec3 position{};
    if ((count >= capacity) ||
        !icon_position(world, entity, &icon.kind, &position) ||
        !math::project_to_ndc(viewProjection, position, &icon.ndc) ||
        (std::fabs(icon.ndc.x) > 1.0F) || (std::fabs(icon.ndc.y) > 1.0F)) {
      return;
    }
    icon.entity = entity;
    out[count++] = icon;
  });
  return count;
}

runtime::Entity pick_icon(const SceneIcon *icons, std::size_t count, float ndcX,
                          float ndcY, float radiusX, float radiusY) noexcept {
  if ((icons == nullptr) || !(radiusX > 0.0F) || !(radiusY > 0.0F)) {
    return runtime::kInvalidEntity;
  }
  runtime::Entity best = runtime::kInvalidEntity;
  float bestDistance = 0.0F;
  float bestDepth = 0.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    // Distance in units of the radius: 1 is the ellipse's edge.
    const float dx = (icons[i].ndc.x - ndcX) / radiusX;
    const float dy = (icons[i].ndc.y - ndcY) / radiusY;
    const float distance = (dx * dx) + (dy * dy);
    if (distance > 1.0F) {
      continue;
    }
    if ((best == runtime::kInvalidEntity) || (distance < bestDistance) ||
        ((distance == bestDistance) && (icons[i].ndc.z < bestDepth))) {
      best = icons[i].entity;
      bestDistance = distance;
      bestDepth = icons[i].ndc.z;
    }
  }
  return best;
}

std::size_t scene_box_select(runtime::World &world,
                             const math::Frustum &frustum,
                             MeshBoundsFn meshBounds, BoxSelectVisit visit,
                             void *context) noexcept {
  std::size_t count = 0U;
  world.for_each_alive([&](runtime::Entity entity) noexcept {
    math::Vec3 center{};
    math::Vec3 half{};
    SceneIconKind kind = SceneIconKind::Light;
    math::Vec3 iconPosition{};
    const bool inside =
        (entity_mesh_world_box(world, entity, meshBounds, &center, &half) &&
         !math::frustum_excludes_box(frustum, center, half)) ||
        (entity_collider_world_box(world, entity, &center, &half) &&
         !math::frustum_excludes_box(frustum, center, half)) ||
        (icon_position(world, entity, &kind, &iconPosition) &&
         !math::frustum_excludes_sphere(frustum, iconPosition, 0.0F));
    if (inside) {
      ++count;
      if (visit != nullptr) {
        visit(context, entity);
      }
    }
  });
  return count;
}

} // namespace engine::editor
