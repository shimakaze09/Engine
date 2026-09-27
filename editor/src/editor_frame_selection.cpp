// Implements Frame Selected, declared in editor_frame_selection.h.

#include "editor_frame_selection.h"

#include "editor_hierarchy_walk.h"
#include "editor_session.h"

#include <algorithm>
#include <cmath>

#include "engine/editor/editor_camera.h"
#include "engine/math/aabb.h"
#include "engine/math/mat4.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/editor_bridge.h"

namespace engine::editor {

namespace {

/// Nesting deeper than this frames only the upper levels, as the
/// Entities panel draws them.
constexpr std::size_t kMaxFrameDepth = 64U;
/// Room around the sphere, so its silhouette does not touch the edges.
constexpr float kFrameMargin = 1.1F;

/// A growing axis-aligned box; empty until the first grow.
struct UnionBox final {
  math::Vec3 min{};
  math::Vec3 max{};
  bool any = false;

  void grow(const math::Vec3 &center, const math::Vec3 &half) noexcept {
    const math::Vec3 lo(center.x - half.x, center.y - half.y,
                        center.z - half.z);
    const math::Vec3 hi(center.x + half.x, center.y + half.y,
                        center.z + half.z);
    if (!any) {
      min = lo;
      max = hi;
      any = true;
      return;
    }
    min = math::Vec3(std::min(min.x, lo.x), std::min(min.y, lo.y),
                     std::min(min.z, lo.z));
    max = math::Vec3(std::max(max.x, hi.x), std::max(max.y, hi.y),
                     std::max(max.z, hi.z));
  }
};

/// Grows `box` by what `entity` draws and collides with, or its position.
void grow_by_entity(runtime::World &world, runtime::Entity entity,
                    MeshBoundsFn meshBounds, UnionBox *box) noexcept {
  const runtime::WorldTransform *worldTransform =
      world.get_world_transform_read_ptr(entity);
  if (worldTransform == nullptr) {
    return;
  }
  bool extent = false;
  math::Vec3 center{};
  math::Vec3 half{};
  if (entity_mesh_world_box(world, entity, meshBounds, &center, &half)) {
    box->grow(center, half);
    extent = true;
  }
  if (entity_collider_world_box(world, entity, &center, &half)) {
    box->grow(center, half);
    extent = true;
  }
  if (!extent) {
    box->grow(math::transform_point(worldTransform->matrix, math::Vec3()),
              math::Vec3());
  }
}

} // namespace

bool selection_framing_sphere(runtime::World &world,
                              const runtime::Entity *entities,
                              std::size_t count, MeshBoundsFn meshBounds,
                              FramingSphere *out) noexcept {
  if ((entities == nullptr) || (out == nullptr)) {
    return false;
  }
  UnionBox box{};
  for (std::size_t i = 0U; i < count; ++i) {
    if (!world.is_alive(entities[i])) {
      continue;
    }
    walk_entity_subtree(world, entities[i], kMaxFrameDepth,
                        [&](runtime::Entity entity) noexcept {
                          grow_by_entity(world, entity, meshBounds, &box);
                        });
  }
  if (!box.any) {
    return false;
  }
  const math::AABB bounds{box.min, box.max};
  out->center = math::aabb_center(bounds);
  out->radius = std::max(math::length(math::aabb_half_extents(bounds)),
                         kFramePointRadius);
  return true;
}

float framing_distance(float radius, float verticalFovRadians,
                       float aspect) noexcept {
  const float halfVertical = 0.5F * verticalFovRadians;
  const float halfHorizontal =
      std::atan(std::tan(halfVertical) * ((aspect > 0.0F) ? aspect : 1.0F));
  const float halfNarrow = std::min(halfVertical, halfHorizontal);
  const float distance = (kFrameMargin * radius) / std::sin(halfNarrow);
  if (!(distance > EditorCamera::kMinDistance)) {
    return EditorCamera::kMinDistance;
  }
  return std::min(distance, EditorCamera::kMaxDistance);
}

bool frame_selection() noexcept {
  EditorSession &session = editor_session();
  if (session.world == nullptr) {
    return false;
  }
  prune_entity_selection();
  const runtime::Entity primary = selected_entity();
  const runtime::Entity *entities = session.selectedEntities.data();
  std::size_t count = session.selectedEntityCount;
  if (count == 0U) {
    entities = &primary;
    count = (primary != runtime::kInvalidEntity) ? 1U : 0U;
  }
  FramingSphere sphere{};
  if (!selection_framing_sphere(*session.world, entities, count,
                                &runtime::editor_mesh_local_bounds, &sphere)) {
    return false;
  }
  const renderer::CameraState camera =
      editor_camera_state(session.editorCamera);
  const float aspect =
      (session.sceneViewPixelHeight > 0)
          ? static_cast<float>(session.sceneViewPixelWidth) /
                static_cast<float>(session.sceneViewPixelHeight)
          : 1.0F;
  session.editorCamera.target = sphere.center;
  session.editorCamera.distance =
      framing_distance(sphere.radius, camera.fovRadians, aspect);
  return true;
}

} // namespace engine::editor
