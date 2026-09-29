// Implements the editor's Scene and Game view panels, the Scene view's
// gizmos and overlays, and the bridge hooks that hand both views to the
// pipeline.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_viewport.h"

#include "editor_commands.h"
#include "editor_entity_menus.h"
#include "editor_grid.h"
#include "editor_light_gizmos.h"
#include "editor_panels_diagnostics.h"
#include "editor_play_recording.h"
#include "editor_scene_query.h"
#include "editor_screenshot.h"
#include "editor_session.h"
#include "editor_shortcuts.h"
#include "editor_transform_util.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/debug_draw.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/editor/editor_camera.h"
#include "engine/engine.h"
#include "engine/math/frustum.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/physics/collider.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/camera_component_update.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/camera_frustum_overlay.h"
#include "engine/editor/command_history.h"

#include <stb_image.h>

namespace engine::editor {

namespace {

constexpr core::DebugColor kColliderWireColor{0.16F, 1.0F, 0.47F, 0.86F};

/// Emits one collider-frame segment as a depth-tested world debug line.
void emit_collider_segment(const math::Mat4 &localToWorld,
                           const math::Vec3 &from,
                           const math::Vec3 &to) noexcept {
  const math::Vec3 worldFrom = math::transform_point(localToWorld, from);
  const math::Vec3 worldTo = math::transform_point(localToWorld, to);
  core::debug_draw_line({worldFrom.x, worldFrom.y, worldFrom.z},
                        {worldTo.x, worldTo.y, worldTo.z}, kColliderWireColor);
}

/// Emits the 12 edges of a collider-frame box around center.
void emit_collider_box(const math::Mat4 &localToWorld,
                       const math::Vec3 &center,
                       const math::Vec3 &halfExtents) noexcept {
  math::Vec3 corners[8]{};
  for (std::size_t corner = 0U; corner < 8U; ++corner) {
    corners[corner] = math::Vec3(
        center.x + (((corner & 1U) != 0U) ? halfExtents.x : -halfExtents.x),
        center.y + (((corner & 2U) != 0U) ? halfExtents.y : -halfExtents.y),
        center.z + (((corner & 4U) != 0U) ? halfExtents.z : -halfExtents.z));
  }

  // Corner indices are bit-encoded (x=1, y=2, z=4); every edge joins two
  // corners that differ in exactly one bit.
  static constexpr int kEdges[24] = {
      0, 1, 2, 3, 4, 5, 6, 7, // x-aligned edges
      0, 2, 1, 3, 4, 6, 5, 7, // y-aligned edges
      0, 4, 1, 5, 2, 6, 3, 7  // z-aligned edges
  };
  for (int i = 0; i < 24; i += 2) {
    emit_collider_segment(localToWorld, corners[kEdges[i]],
                          corners[kEdges[i + 1]]);
  }
}

/// Emits an arc of the given sweep in one collider-frame plane.
void emit_collider_arc(const math::Mat4 &localToWorld,
                       const math::Vec3 &center, float radius, int axisU,
                       int axisV, float startAngle, float sweep) noexcept {
  constexpr int kSegments = 16;
  math::Vec3 previous{};
  for (int i = 0; i <= kSegments; ++i) {
    const float angle =
        startAngle + (sweep * static_cast<float>(i)) / kSegments;
    float coords[3] = {center.x, center.y, center.z};
    coords[axisU] += radius * std::cos(angle);
    coords[axisV] += radius * std::sin(angle);
    const math::Vec3 point(coords[0], coords[1], coords[2]);
    if (i > 0) {
      emit_collider_segment(localToWorld, previous, point);
    }
    previous = point;
  }
}

/// Emits a full circle in one collider-frame plane.
void emit_collider_circle(const math::Mat4 &localToWorld,
                          const math::Vec3 &center, float radius, int axisU,
                          int axisV) noexcept {
  constexpr float kTwoPi = 6.28318530718F;
  emit_collider_arc(localToWorld, center, radius, axisU, axisV, 0.0F, kTwoPi);
}

// Emits a convex hull's crease edges: vertex pairs lying on two hull faces
// with distinct normals. Coplanar triangulation diagonals stay invisible.
void emit_collider_hull(const math::Mat4 &localToWorld,
                        const physics::ConvexHullData &hull) noexcept {
  constexpr float kOnPlaneEpsilon = 1.0e-3F;
  constexpr float kCoplanarNormalDot = 0.9999F;
  for (std::size_t u = 0U; u + 1U < hull.vertexCount; ++u) {
    for (std::size_t v = u + 1U; v < hull.vertexCount; ++v) {
      const math::Vec3 &from = hull.vertices[u];
      const math::Vec3 &to = hull.vertices[v];

      const physics::ConvexHullData::Plane *firstFace = nullptr;
      bool crease = false;
      for (std::size_t p = 0U; (p < hull.planeCount) && !crease; ++p) {
        const physics::ConvexHullData::Plane &plane = hull.planes[p];
        if ((std::fabs(math::dot(plane.normal, from) - plane.distance) >
             kOnPlaneEpsilon) ||
            (std::fabs(math::dot(plane.normal, to) - plane.distance) >
             kOnPlaneEpsilon)) {
          continue;
        }
        if (firstFace == nullptr) {
          firstFace = &plane;
        } else if (math::dot(firstFace->normal, plane.normal) <
                   kCoplanarNormalDot) {
          crease = true;
        }
      }
      if (crease) {
        emit_collider_segment(localToWorld, from, to);
      }
    }
  }
}

// Shows the selected entity's authored CameraComponent as a frustum
// wireframe: derives the same pose/lens update_persistent_
// cameras would publish (runtime::camera_component_pose keeps the -Z-
// forward/+Y-up convention in one place) and reuses the existing frozen-
// game-camera frustum drawer, so the gizmo always matches what the entity
// would actually render if it became the active game camera. Uses the
// viewport's own aspect ratio, which may differ slightly from the game's
// presented aspect -- the frustum shape stays representative either way.
void draw_selected_camera_frustum_overlay(const runtime::Entity selectedEntity,
                                          float aspectRatio) noexcept {
  if ((editor_session().world == nullptr) ||
      (selectedEntity == runtime::kInvalidEntity) || (aspectRatio <= 0.0F)) {
    return;
  }

  renderer::CameraState pose{};
  if (!runtime::camera_component_pose(*editor_session().world, selectedEntity,
                                      &pose)) {
    return;
  }
  draw_camera_frustum_wireframe(pose, aspectRatio);
}

// Emits the selected entity's collider as a shape-matched wireframe into the
// depth-tested debug line pass, using the same world geometry physics
// collides with.
void draw_selected_collider_overlay(
    const runtime::Entity selectedEntity) noexcept {
  if ((editor_session().world == nullptr) ||
      (selectedEntity == runtime::kInvalidEntity)) {
    return;
  }

  const runtime::Collider *collider =
      editor_session().world->get_collider_ptr(selectedEntity);
  if (collider == nullptr) {
    return;
  }

  const runtime::WorldTransform *worldTransform =
      editor_session().world->get_world_transform_read_ptr(selectedEntity);
  if (worldTransform == nullptr) {
    return;
  }

  const physics::ConvexHullData *hull =
      (collider->shape == runtime::ColliderShape::ConvexHull)
          ? runtime::get_convex_hull_data(*editor_session().world,
                                          selectedEntity)
          : nullptr;
  physics::ColliderWorldGeometry geometry{};
  if (!physics::make_collider_world_geometry(*collider, worldTransform->matrix,
                                             hull, &geometry)) {
    return;
  }

  const math::Mat4 &localToWorld = geometry.localToWorld;
  const math::Vec3 origin(0.0F, 0.0F, 0.0F);
  constexpr float kPi = 3.14159265359F;

  switch (collider->shape) {
  case runtime::ColliderShape::Sphere: {
    const float radius = collider->halfExtents.x;
    emit_collider_circle(localToWorld, origin, radius, 0, 1);
    emit_collider_circle(localToWorld, origin, radius, 0, 2);
    emit_collider_circle(localToWorld, origin, radius, 2, 1);
    break;
  }
  case runtime::ColliderShape::Capsule: {
    const float radius = collider->halfExtents.x;
    const float halfHeight = collider->halfExtents.y;
    const math::Vec3 top(0.0F, halfHeight, 0.0F);
    const math::Vec3 bottom(0.0F, -halfHeight, 0.0F);

    emit_collider_circle(localToWorld, top, radius, 0, 2);
    emit_collider_circle(localToWorld, bottom, radius, 0, 2);
    emit_collider_segment(localToWorld, math::Vec3(radius, -halfHeight, 0.0F),
                          math::Vec3(radius, halfHeight, 0.0F));
    emit_collider_segment(localToWorld, math::Vec3(-radius, -halfHeight, 0.0F),
                          math::Vec3(-radius, halfHeight, 0.0F));
    emit_collider_segment(localToWorld, math::Vec3(0.0F, -halfHeight, radius),
                          math::Vec3(0.0F, halfHeight, radius));
    emit_collider_segment(localToWorld, math::Vec3(0.0F, -halfHeight, -radius),
                          math::Vec3(0.0F, halfHeight, -radius));

    emit_collider_arc(localToWorld, top, radius, 0, 1, 0.0F, kPi);
    emit_collider_arc(localToWorld, top, radius, 2, 1, 0.0F, kPi);
    emit_collider_arc(localToWorld, bottom, radius, 0, 1, kPi, kPi);
    emit_collider_arc(localToWorld, bottom, radius, 2, 1, kPi, kPi);
    break;
  }
  case runtime::ColliderShape::ConvexHull: {
    if ((hull != nullptr) && (hull->vertexCount >= 4U) &&
        (hull->planeCount >= 4U)) {
      emit_collider_hull(localToWorld, *hull);
    } else if (hull != nullptr) {
      emit_collider_box(localToWorld, hull->localCenter,
                        hull->localHalfExtents);
    } else {
      emit_collider_box(localToWorld, origin, collider->halfExtents);
    }
    break;
  }
  case runtime::ColliderShape::Heightfield: {
    // Heightfields are world-anchored: draw the conservative world bounds.
    const math::Vec3 aabbCenter =
        math::mul(math::add(geometry.worldAabb.min, geometry.worldAabb.max),
                  0.5F);
    const math::Vec3 aabbHalf = math::mul(
        math::sub(geometry.worldAabb.max, geometry.worldAabb.min), 0.5F);
    emit_collider_box(math::Mat4(), aabbCenter, aabbHalf);
    break;
  }
  default: {
    emit_collider_box(localToWorld, origin, collider->halfExtents);
    break;
  }
  }
}

/// Icons one Scene view draws; lights and cameras beyond this many in
/// view go undrawn.
constexpr std::size_t kMaxSceneIcons = 256U;

/// The Scene camera's view and projection for an image of `imageSize`
/// (positive), through the renderer's projection builder, so every Scene
/// view tool projects with the camera the view renders.
struct SceneMatrices final {
  math::Mat4 view{};
  math::Mat4 projection{};
};

SceneMatrices scene_view_matrices(const ImVec2 &imageSize) noexcept {
  const renderer::CameraState cam =
      editor_camera_state(editor_session().editorCamera);
  return SceneMatrices{
      math::look_at(cam.position, cam.target, cam.up),
      renderer::camera_projection_matrix(cam, imageSize.x / imageSize.y)};
}

/// A point on the Scene image as normalized device coordinates, +y up.
ImVec2 image_to_ndc(const ImVec2 &point, const ImVec2 &imagePos,
                    const ImVec2 &imageSize) noexcept {
  return ImVec2(((2.0F * (point.x - imagePos.x)) / imageSize.x) - 1.0F,
                1.0F - ((2.0F * (point.y - imagePos.y)) / imageSize.y));
}

/// Normalized device coordinates as a point on the Scene image.
ImVec2 ndc_to_image(float ndcX, float ndcY, const ImVec2 &imagePos,
                    const ImVec2 &imageSize) noexcept {
  return ImVec2(imagePos.x + ((ndcX + 1.0F) * 0.5F * imageSize.x),
                imagePos.y + ((1.0F - ndcY) * 0.5F * imageSize.y));
}

/// The icons of the lights and cameras the Scene image at (imagePos,
/// imageSize) shows; see scene_icons.
std::size_t collect_scene_icons(const ImVec2 &imageSize, SceneIcon *out,
                                std::size_t capacity) noexcept {
  const EditorSession &session = editor_session();
  if ((session.world == nullptr) || (imageSize.x <= 0.0F) ||
      (imageSize.y <= 0.0F)) {
    return 0U;
  }
  const SceneMatrices matrices = scene_view_matrices(imageSize);
  return scene_icons(*session.world,
                     math::mul(matrices.projection, matrices.view), out,
                     capacity);
}

/// Draws the Scene view's light and camera icons over its image, as
/// Unity's gizmo icons: screen-sized glyphs that geometry does not hide,
/// ringed when selected.
void draw_scene_icons(const ImVec2 &imagePos,
                      const ImVec2 &imageSize) noexcept {
  std::array<SceneIcon, kMaxSceneIcons> icons =
      std::array<SceneIcon, kMaxSceneIcons>();
  const std::size_t count =
      collect_scene_icons(imageSize, icons.data(), icons.size());
  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const SceneIconMetrics metrics =
      scene_icon_metrics(editor_session().uiScale, editor_session().iconScale);
  // Every shape is a fraction of the icon's radius, so the whole glyph
  // scales with it and stays within the radius it is picked by.
  const float r = metrics.radius;
  constexpr ImU32 kLightColor = IM_COL32(255, 220, 90, 230);
  constexpr ImU32 kCameraColor = IM_COL32(150, 200, 255, 230);
  constexpr ImU32 kOutline = IM_COL32(20, 20, 20, 200);
  for (std::size_t i = 0U; i < count; ++i) {
    const ImVec2 c =
        ndc_to_image(icons[i].ndc.x, icons[i].ndc.y, imagePos, imageSize);
    if (icons[i].kind == SceneIconKind::Light) {
      // A sun: a disc with eight rays.
      for (int ray = 0; ray < 8; ++ray) {
        const float angle = 0.78539816F * static_cast<float>(ray);
        const ImVec2 dir(std::cos(angle), std::sin(angle));
        drawList->AddLine(
            ImVec2(c.x + (dir.x * r * 0.66F), c.y + (dir.y * r * 0.66F)),
            ImVec2(c.x + (dir.x * r * 0.95F), c.y + (dir.y * r * 0.95F)),
            kLightColor, metrics.stroke);
      }
      drawList->AddCircleFilled(c, r * 0.5F, kLightColor);
      drawList->AddCircle(c, r * 0.5F, kOutline, 0, metrics.stroke * 0.66F);
    } else {
      // A camera: a body with a lens to its right.
      const ImVec2 bodyMin(c.x - (r * 0.8F), c.y - (r * 0.5F));
      const ImVec2 bodyMax(c.x + (r * 0.3F), c.y + (r * 0.5F));
      drawList->AddRectFilled(bodyMin, bodyMax, kCameraColor, r * 0.15F);
      drawList->AddTriangleFilled(
          ImVec2(bodyMax.x, c.y), ImVec2(c.x + (r * 0.9F), bodyMin.y),
          ImVec2(c.x + (r * 0.9F), bodyMax.y), kCameraColor);
      drawList->AddRect(bodyMin, bodyMax, kOutline, r * 0.15F, 0,
                        metrics.stroke * 0.66F);
    }
    if (is_entity_selected(icons[i].entity) ||
        (selected_entity() == icons[i].entity)) {
      drawList->AddCircle(c, metrics.selectionRadius,
                          IM_COL32(255, 255, 255, 220), 0, metrics.stroke);
    }
  }
}

bool point_in_rect(const ImVec2 &point, const ImVec2 &min,
                   const ImVec2 &size) noexcept {
  return (point.x >= min.x) && (point.y >= min.y) &&
         (point.x < (min.x + size.x)) && (point.y < (min.y + size.y));
}

/// The Scene view ray under `mouse` in the Scene image at (imagePos,
/// imageSize), with a unit direction and the near-to-far span as `reach`;
/// false when the image or the camera cannot give one.
bool scene_view_ray(const ImVec2 &mouse, const ImVec2 &imagePos,
                    const ImVec2 &imageSize, math::Ray *ray,
                    float *reach) noexcept {
  if ((imageSize.x <= 0.0F) || (imageSize.y <= 0.0F)) {
    return false;
  }
  const ImVec2 ndc = image_to_ndc(mouse, imagePos, imageSize);
  const SceneMatrices matrices = scene_view_matrices(imageSize);
  if (!viewport_ray(matrices.view, matrices.projection,
                    renderer::device_depth_zero_one(), ndc.x, ndc.y, ray)) {
    return false;
  }
  // The ray runs from the near plane to the far plane; queries take a
  // unit direction and that span as their reach.
  *reach = math::length(ray->direction);
  if (!(*reach > 0.0F)) {
    return false;
  }
  ray->direction = math::mul(ray->direction, 1.0F / *reach);
  return true;
}

/// What lies under `mouse` in the Scene image; see editor_scene_query.h.
/// Icons are picked before geometry, as Unity's gizmo icons are: a light
/// inside a lamp mesh is reachable by its icon. `walkOverlaps` lets a
/// click on the spot of the last one pick the next hit behind the current
/// selection. kInvalidEntity over empty space.
runtime::Entity entity_under_cursor(const ImVec2 &mouse, const ImVec2 &imagePos,
                                    const ImVec2 &imageSize,
                                    bool walkOverlaps) noexcept {
  EditorSession &session = editor_session();
  if (session.world == nullptr) {
    return runtime::kInvalidEntity;
  }
  const ImVec2 ndc = image_to_ndc(mouse, imagePos, imageSize);
  std::array<SceneIcon, kMaxSceneIcons> icons =
      std::array<SceneIcon, kMaxSceneIcons>();
  const std::size_t iconCount =
      collect_scene_icons(imageSize, icons.data(), icons.size());
  const SceneIconMetrics iconMetrics =
      scene_icon_metrics(session.uiScale, session.iconScale);
  const runtime::Entity iconPick =
      pick_icon(icons.data(), iconCount, ndc.x, ndc.y,
                (2.0F * iconMetrics.radius) / imageSize.x,
                (2.0F * iconMetrics.radius) / imageSize.y);
  if (iconPick != runtime::kInvalidEntity) {
    session.hasLastPick = false;
    return iconPick;
  }
  math::Ray ray{};
  float reach = 0.0F;
  if (!scene_view_ray(mouse, imagePos, imageSize, &ray, &reach)) {
    return runtime::kInvalidEntity;
  }
  std::array<PickHit, 32> hits{};
  const std::size_t count = scene_pick_hits(*session.world, ray, reach,
                                            &runtime::editor_mesh_local_bounds,
                                            hits.data(), hits.size());
  const bool sameSpot =
      session.hasLastPick && within_click_slop(mouse.x - session.lastPickPos.x,
                                               mouse.y - session.lastPickPos.y);
  session.hasLastPick = true;
  session.lastPickPos = mouse;
  return choose_pick(hits.data(), count, selected_entity(),
                     sameSpot && walkOverlaps);
}

/// Picks what lies under `mouse` in the Scene image at (imagePos,
/// imageSize). `additive` (Ctrl) toggles the pick in or out of the
/// selection; otherwise it replaces the selection, and a click on empty
/// space clears it.
void pick_in_scene_view(const ImVec2 &mouse, const ImVec2 &imagePos,
                        const ImVec2 &imageSize, bool additive) noexcept {
  if ((editor_session().world == nullptr) || (imageSize.x <= 0.0F) ||
      (imageSize.y <= 0.0F)) {
    return;
  }
  const runtime::Entity picked =
      entity_under_cursor(mouse, imagePos, imageSize, !additive);
  if (picked == runtime::kInvalidEntity) {
    if (!additive) {
      clear_entity_selection();
    }
    return;
  }
  select_entity(picked, additive);
}

constexpr const char *kSceneViewMenu = "scene_view_menu";

/// Opens the Scene view's menu for a right-click at `mouse`: on the entity
/// under it, which joins the selection, or on empty space, recording the
/// ground point a creation goes to.
void open_scene_view_menu(const ImVec2 &mouse, const ImVec2 &imagePos,
                          const ImVec2 &imageSize) noexcept {
  EditorSession &session = editor_session();
  if (session.world == nullptr) {
    return;
  }
  session.sceneMenuEntity =
      entity_under_cursor(mouse, imagePos, imageSize, false);
  if ((session.sceneMenuEntity != runtime::kInvalidEntity) &&
      !is_entity_selected(session.sceneMenuEntity) &&
      (selected_entity() != session.sceneMenuEntity)) {
    select_entity(session.sceneMenuEntity, false);
  }
  math::Ray ray{};
  float reach = 0.0F;
  session.sceneMenuHasGround =
      scene_view_ray(mouse, imagePos, imageSize, &ray, &reach) &&
      ray_ground_point(ray, reach, &session.sceneMenuGround);
  ImGui::OpenPopup(kSceneViewMenu);
}

/// Draws the Scene view's menu while it is open, and runs its choice: on
/// an entity its edits and children, on empty space creation at the
/// ground point (the camera's focus when the click missed the ground).
void draw_scene_view_menu() noexcept {
  if (!ImGui::BeginPopup(kSceneViewMenu)) {
    return;
  }
  EditorSession &session = editor_session();
  const bool onEntity = (session.world != nullptr) &&
                        session.world->is_alive(session.sceneMenuEntity);
  const EntityMenuChoice choice =
      onEntity ? draw_entity_menu_items() : draw_empty_space_menu_items();
  ImGui::EndPopup();
  EntitySpawnPlacement placement{};
  if (onEntity) {
    placement.parent = session.sceneMenuEntity;
  } else if (session.sceneMenuHasGround) {
    placement.hasPosition = true;
    placement.position = session.sceneMenuGround;
  }
  static_cast<void>(run_entity_menu_choice(choice, placement));
}

/// Adds `entity` to the selection unless it is already a member.
void add_to_selection(void *, runtime::Entity entity) noexcept {
  if (!is_entity_selected(entity)) {
    select_entity(entity, true);
  }
}

/// Selects what the marquee from `from` to `to` covers in the Scene image
/// at (imagePos, imageSize), as Unity's rectangle selection does:
/// `additive` (Shift or Ctrl) adds it to the selection, otherwise it
/// replaces the selection. An empty marquee clears it unless additive.
void box_select_in_scene_view(const ImVec2 &from, const ImVec2 &to,
                              const ImVec2 &imagePos, const ImVec2 &imageSize,
                              bool additive) noexcept {
  EditorSession &session = editor_session();
  if ((session.world == nullptr) || (imageSize.x <= 0.0F) ||
      (imageSize.y <= 0.0F)) {
    return;
  }
  // The marquee in NDC, clipped to the image.
  const auto clamp_ndc = [](float value) noexcept {
    return std::fmax(-1.0F, std::fmin(1.0F, value));
  };
  const ImVec2 a = image_to_ndc(from, imagePos, imageSize);
  const ImVec2 b = image_to_ndc(to, imagePos, imageSize);
  const float minX = clamp_ndc(std::fmin(a.x, b.x));
  const float maxX = clamp_ndc(std::fmax(a.x, b.x));
  const float minY = clamp_ndc(std::fmin(a.y, b.y));
  const float maxY = clamp_ndc(std::fmax(a.y, b.y));
  if (!additive) {
    clear_entity_selection();
  }
  if (!(maxX > minX) || !(maxY > minY)) {
    return; // clipped away entirely
  }
  const SceneMatrices matrices = scene_view_matrices(imageSize);
  const math::Frustum marquee = math::frustum_from_view_projection(
      math::mul(math::sub_rect_projection(matrices.projection, minX, minY, maxX,
                                          maxY),
                matrices.view),
      renderer::device_depth_zero_one());
  static_cast<void>(scene_box_select(*session.world, marquee,
                                     &runtime::editor_mesh_local_bounds,
                                     &add_to_selection, nullptr));
}

/// Projects the current mouse position through the editor camera onto the
/// y = 0 ground plane (falling back to a point ahead of the camera when
/// the ray misses it within the far plane) to place viewport asset drops.
math::Vec3 viewport_drop_world_position(const ImVec2 &imagePos,
                                        const ImVec2 &imageSize) noexcept {
  const renderer::CameraState cam =
      editor_camera_state(editor_session().editorCamera);
  const math::Vec3 forward =
      math::normalize(math::sub(cam.target, cam.position));
  constexpr float kFallbackDistance = 6.0F;
  const math::Vec3 fallback =
      math::add(cam.position, math::mul(forward, kFallbackDistance));
  if ((imageSize.x <= 0.0F) || (imageSize.y <= 0.0F)) {
    return fallback;
  }

  const ImVec2 ndc = image_to_ndc(ImGui::GetMousePos(), imagePos, imageSize);
  const SceneMatrices matrices = scene_view_matrices(imageSize);
  math::Ray ray{};
  if (!viewport_ray(matrices.view, matrices.projection,
                    renderer::device_depth_zero_one(), ndc.x, ndc.y, &ray) ||
      (ray.direction.y >= 0.0F)) {
    return fallback;
  }
  // The ray spans near to far plane at t in [0, 1]; a ground hit past the
  // far plane is not on screen.
  const float t = -ray.origin.y / ray.direction.y;
  if ((t < 0.0F) || (t > 1.0F)) {
    return fallback;
  }
  return math::add(ray.origin, math::mul(ray.direction, t));
}

} // namespace

/// Draws a render view's last image filling the panel's content region.
void draw_view_image(renderer::RenderViewId view,
                     const ImVec2 &regionSize) noexcept {
  const std::uint64_t texId =
      imgui_texture_id(renderer::get_render_view_texture(view));
  if ((texId != 0U) && (regionSize.x > 0.0F) && (regionSize.y > 0.0F)) {
    // The pass chain is hop-neutral (see fullscreen.vs.sc), so display
    // parity reduces to the backend's render-target row order: GL-family
    // targets store rows bottom-up and need the classic V flip, y-down
    // targets sample upright.
    const bool flipV = renderer::device_target_origin_bottom_left();
    ImGui::Image(static_cast<ImTextureID>(texId), regionSize,
                 ImVec2(0.0F, flipV ? 1.0F : 0.0F),
                 ImVec2(1.0F, flipV ? 0.0F : 1.0F));
  } else if ((regionSize.x > 0.0F) && (regionSize.y > 0.0F)) {
    // No image this frame (a Game view with no Camera, or a view not yet
    // rendered): the region is black, never an older frame.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        origin, ImVec2(origin.x + regionSize.x, origin.y + regionSize.y),
        IM_COL32(0, 0, 0, 255));
    ImGui::Dummy(regionSize);
  }
}

/// A panel's content size in pixels: the rect is in logical points, and
/// the render target is sized in pixels so HiDPI displays get a
/// native-resolution image.
void region_pixels(const ImVec2 &regionSize, int *outWidth,
                   int *outHeight) noexcept {
  const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
  *outWidth = static_cast<int>(regionSize.x * fbScale.x);
  *outHeight = static_cast<int>(regionSize.y * fbScale.y);
}

void draw_scene_viewport_panel() noexcept {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
  const bool visible = ImGui::Begin(kSceneViewWindow);
  ImGui::PopStyleVar();

  editor_session().sceneViewShown = visible;
  if (!visible) {
    ImGui::End();
    return;
  }

  const ImVec2 regionSize = ImGui::GetContentRegionAvail();
  const ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();

  editor_session().sceneViewportScreenPos = cursorScreenPos;
  editor_session().sceneViewportScreenSize = regionSize;
  region_pixels(regionSize, &editor_session().sceneViewPixelWidth,
                &editor_session().sceneViewPixelHeight);

  draw_view_image(renderer::RenderViewId::Scene, regionSize);
  draw_scene_icons(cursorScreenPos, regionSize);

  // Dropping a browser mesh asset spawns it where the drop ray meets the
  // ground plane, as an undoable create.
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload("ASSET_VIRTUAL_PATH")) {
      char virtualPath[512] = {};
      if ((payload->Data != nullptr) && (payload->DataSize > 0) &&
          (static_cast<std::size_t>(payload->DataSize) <=
           sizeof(virtualPath)) &&
          world_is_editable()) {
        std::memcpy(virtualPath, payload->Data,
                    static_cast<std::size_t>(payload->DataSize));
        virtualPath[sizeof(virtualPath) - 1U] = '\0';
        runtime::Transform spawnTransform{};
        spawnTransform.position =
            viewport_drop_world_position(cursorScreenPos, regionSize);
        const runtime::Entity spawned =
            execute_asset_spawn(virtualPath, spawnTransform);
        if (spawned != runtime::kInvalidEntity) {
          select_entity(spawned, false);
        }
      }
    }
    ImGui::EndDragDropTarget();
  }

  const bool editable = world_is_editable();
  const runtime::Entity selectedEntity = selected_entity();

  const bool hasTransform = (selectedEntity != runtime::kInvalidEntity) &&
                            (editor_session().world != nullptr) &&
                            (editor_session().world->get_transform_read_ptr(
                                 selectedEntity) != nullptr);

  if (editor_session().showGrid) {
    const EditorCamera &orbit = editor_session().editorCamera;
    emit_reference_grid(orbit.target, orbit.distance);
  }
  if (selectedEntity != runtime::kInvalidEntity) {
    draw_selected_collider_overlay(selectedEntity);
    if (editor_session().world != nullptr) {
      draw_light_gizmos(*editor_session().world, selectedEntity);
    }
    if (regionSize.y > 0.0F) {
      draw_selected_camera_frustum_overlay(selectedEntity,
                                           regionSize.x / regionSize.y);
    }
  }

  bool gizmoDrawn = false;
  if (editable && hasTransform && (regionSize.x > 0.0F) &&
      (regionSize.y > 0.0F)) {
    gizmoDrawn = true;
    // ImGuizmo reads only x and y and unprojects its picking ray between
    // clip depths 0 and 1, which lie on the view ray under either depth
    // convention.
    const SceneMatrices matrices = scene_view_matrices(regionSize);
    const math::Mat4 &viewMat = matrices.view;
    const math::Mat4 &projMat = matrices.projection;

    runtime::Transform transform{};
    editor_session().world->get_transform(selectedEntity, &transform);
    const runtime::WorldTransform *worldTransform =
        editor_session().world->get_world_transform_read_ptr(selectedEntity);
    math::Mat4 modelMat =
        (worldTransform != nullptr)
            ? worldTransform->matrix
            : math::compose_trs(transform.position, transform.rotation,
                                transform.scale);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(cursorScreenPos.x, cursorScreenPos.y, regionSize.x,
                      regionSize.y);

    float snapValues[3] = {0.0F, 0.0F, 0.0F};
    const float *snap = nullptr;
    if (editor_session().snapEnabled) {
      const float step = (editor_session().gizmoOp == ImGuizmo::ROTATE)
                             ? editor_session().snapAngleDegrees
                             : editor_session().snapStep;
      snapValues[0] = step;
      snapValues[1] = step;
      snapValues[2] = step;
      snap = snapValues;
    }

    const bool manipulated = ImGuizmo::Manipulate(
        &viewMat.columns[0].x, &projMat.columns[0].x, editor_session().gizmoOp,
        editor_session().gizmoWorldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
        &modelMat.columns[0].x, nullptr, snap);

    // Fed the pre-manipulation transform so an opening gesture records
    // the pose the drag started from; the closing frame reads the final
    // pose from the world itself.
    const bool gizmoUsing = ImGuizmo::IsUsing();
    gizmo_track_gesture(selectedEntity, gizmoUsing, transform);

    if (manipulated) {
      const math::Mat4 *parentWorldMatrix = nullptr;
      if (transform.parentId != runtime::kInvalidPersistentId) {
        const runtime::Entity parent =
            editor_session().world->find_entity_by_persistent_id(
                transform.parentId);
        const runtime::WorldTransform *parentWorld =
            editor_session().world->get_world_transform_read_ptr(parent);
        if (parentWorld != nullptr) {
          parentWorldMatrix = &parentWorld->matrix;
        }
      }

      runtime::Transform localTransform{};
      if (!world_matrix_to_local_transform(modelMat, parentWorldMatrix,
                                           transform, &localTransform)) {
        core::log_message(
            core::LogLevel::Warning, "editor",
            "gizmo transform could not be converted to local space");
      } else if (!editor_session().world->add_transform(selectedEntity,
                                                        localTransform)) {
        core::log_message(core::LogLevel::Error, "editor",
                          "gizmo transform update failed");
      } else {
        transform = localTransform;
      }
    }

  }

  // A left click picks, as in Unity's Scene view: pressed over the image
  // (not on the gizmo, not with Alt, which orbits) and released within a
  // few pixels of where it went down; a drag is a marquee instead.
  {
    EditorSession &clickSession = editor_session();
    const ImGuiIO &io = ImGui::GetIO();
    const bool overGizmo =
        gizmoDrawn && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());
    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt &&
        !clickSession.sceneFlying && !overGizmo &&
        point_in_rect(io.MousePos, cursorScreenPos, regionSize)) {
      clickSession.scenePressPending = true;
      clickSession.scenePressPos = io.MousePos;
    }
    const bool dragged =
        !within_click_slop(io.MousePos.x - clickSession.scenePressPos.x,
                           io.MousePos.y - clickSession.scenePressPos.y);
    if (clickSession.scenePressPending && dragged) {
      // The marquee, clipped to the image.
      const ImVec2 lo(
          std::fmax(std::fmin(clickSession.scenePressPos.x, io.MousePos.x),
                    cursorScreenPos.x),
          std::fmax(std::fmin(clickSession.scenePressPos.y, io.MousePos.y),
                    cursorScreenPos.y));
      const ImVec2 hi(
          std::fmin(std::fmax(clickSession.scenePressPos.x, io.MousePos.x),
                    cursorScreenPos.x + regionSize.x),
          std::fmin(std::fmax(clickSession.scenePressPos.y, io.MousePos.y),
                    cursorScreenPos.y + regionSize.y));
      ImDrawList *drawList = ImGui::GetWindowDrawList();
      drawList->AddRectFilled(lo, hi, IM_COL32(90, 150, 255, 40));
      drawList->AddRect(lo, hi, IM_COL32(90, 150, 255, 200));
    }
    if (clickSession.scenePressPending &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      clickSession.scenePressPending = false;
      if (!dragged) {
        pick_in_scene_view(clickSession.scenePressPos, cursorScreenPos,
                           regionSize, io.KeyCtrl);
      } else {
        box_select_in_scene_view(clickSession.scenePressPos, io.MousePos,
                                 cursorScreenPos, regionSize,
                                 io.KeyShift || io.KeyCtrl);
      }
    }
  }

  // Flythrough, as in Unity: the right button pressed over the Scene view
  // starts it and releasing it anywhere ends it, so a drag that leaves the
  // panel keeps flying. A release within the click slop of the press is a
  // right-click instead, which opens the Scene view's menu, as Unreal's
  // viewport does.
  EditorSession &flySession = editor_session();
  if (flySession.sceneFlying && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    flySession.sceneFlying = false;
    const ImVec2 released = ImGui::GetIO().MousePos;
    if (flySession.sceneRightPressPending &&
        within_click_slop(released.x - flySession.sceneRightPressPos.x,
                          released.y - flySession.sceneRightPressPos.y)) {
      open_scene_view_menu(released, cursorScreenPos, regionSize);
    }
    flySession.sceneRightPressPending = false;
  }
  if (!flySession.sceneFlying && ImGui::IsWindowHovered() &&
      !ImGuizmo::IsUsing() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    flySession.sceneFlying = true;
    flySession.sceneRightPressPending =
        point_in_rect(ImGui::GetIO().MousePos, cursorScreenPos, regionSize);
    flySession.sceneRightPressPos = ImGui::GetIO().MousePos;
  }
  draw_scene_view_menu();
  if (flySession.sceneFlying) {
    const ImGuiIO &io = ImGui::GetIO();
    const auto axis = [](ImGuiKey plus, ImGuiKey minus) noexcept {
      return (ImGui::IsKeyDown(plus) ? 1 : 0) -
             (ImGui::IsKeyDown(minus) ? 1 : 0);
    };
    FlyInput fly{};
    fly.lookX = io.MouseDelta.x;
    fly.lookY = io.MouseDelta.y;
    fly.forward = axis(ImGuiKey_W, ImGuiKey_S);
    fly.right = axis(ImGuiKey_D, ImGuiKey_A);
    fly.up = axis(ImGuiKey_E, ImGuiKey_Q);
    fly.boost = io.KeyShift;
    fly.wheel = (io.MouseWheel > 0.0F) ? 1 : ((io.MouseWheel < 0.0F) ? -1 : 0);
    const float speedBefore = flySession.editorCamera.flySpeed;
    fly_editor_camera(flySession.editorCamera, fly, io.DeltaTime);
    if (flySession.editorCamera.flySpeed != speedBefore) {
      ImGui::MarkIniSettingsDirty(); // the speed is a saved preference
    }
  } else if (ImGui::IsWindowHovered() && !ImGuizmo::IsUsing()) {
    // Orbit, pan and zoom whenever the Scene view is hovered and no gizmo
    // drag holds the mouse, during play as well: the Scene view is the
    // author's, and the game has its own view.
    const ImGuiIO &io = ImGui::GetIO();
    const bool altHeld = io.KeyAlt;
    const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    const int scrollDelta =
        (io.MouseWheel > 0.0F) ? 1 : ((io.MouseWheel < 0.0F) ? -1 : 0);

    update_editor_camera(editor_session().editorCamera,
                         static_cast<int>(io.MouseDelta.x),
                         static_cast<int>(io.MouseDelta.y), scrollDelta,
                         altHeld && lmbDown, altHeld && mmbDown);
  }

  ImGui::End();
}

namespace {

/// The Game view's own toolbar row above its image, as Unity's Game view
/// has one: the Take Screenshot button. Its tooltip is not drawn in the
/// frame a screenshot is taken, where it could overlap the image.
void draw_game_view_toolbar(bool capturing) noexcept {
  // Offset from where the content starts: a docked window's origin lies
  // under its tab bar.
  const ImGuiStyle &style = ImGui::GetStyle();
  ImGui::SetCursorPos(
      ImVec2(ImGui::GetCursorPosX() + style.ItemSpacing.x,
             ImGui::GetCursorPosY() + (style.ItemSpacing.y * 0.5F)));
  const bool enabled = editor_action_enabled(EditorAction::Screenshot);
  ImGui::BeginDisabled(!enabled);
  if (ImGui::SmallButton("Screenshot")) {
    static_cast<void>(run_editor_action(EditorAction::Screenshot));
  }
  ImGui::EndDisabled();
  if (!capturing && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                                         ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Save the Game view as a PNG in the project's "
                      "Screenshots folder (%s)",
                      editor_shortcut_text(EditorAction::Screenshot));
  }
}

constexpr const char *kGameViewMenu = "game_view_menu";

/// The Game view's own menu: its toolbar's actions and the stats overlay.
/// A right-click on the toolbar row opens it; the image belongs to the
/// game, which needs its right mouse button, so a right-click there opens
/// nothing, as neither Unity's Game view nor Unreal's play viewport takes
/// one. Nothing opens in the frame a screenshot is taken.
void draw_game_view_menu(float imageTop, bool capturing) noexcept {
  if (!capturing && ImGui::IsWindowHovered() &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      (ImGui::GetIO().MousePos.y < imageTop)) {
    ImGui::OpenPopup(kGameViewMenu);
  }
  if (!ImGui::BeginPopup(kGameViewMenu)) {
    return;
  }
  editor_action_menu_item(EditorAction::Screenshot);
  ImGui::Separator();
  draw_recording_menu_items();
  ImGui::Separator();
  bool showStats = core::cvar_get_bool(kShowStatsCvar, false);
  if (ImGui::MenuItem("Stats", nullptr, &showStats)) {
    static_cast<void>(core::cvar_set_bool(kShowStatsCvar, showStats));
  }
  ImGui::EndPopup();
}

/// The Game view's camera notice, centred over its image in a dark box;
/// with no camera at all it also offers Create Camera, as the one-click
/// fix, when the world can be edited.
void draw_game_camera_notice(const char *notice, bool offerCreate,
                             const ImVec2 &imagePos,
                             const ImVec2 &regionSize) noexcept {
  const ImGuiStyle &style = ImGui::GetStyle();
  const float wrapWidth = regionSize.x * 0.8F;
  const ImVec2 textSize =
      ImGui::CalcTextSize(notice, nullptr, false, wrapWidth);
  const char *buttonLabel = "Create Camera";
  const ImVec2 buttonSize(ImGui::CalcTextSize(buttonLabel).x +
                              (style.FramePadding.x * 2.0F),
                          ImGui::GetFrameHeight());
  const float blockHeight =
      textSize.y + (offerCreate ? (style.ItemSpacing.y + buttonSize.y) : 0.0F);
  const ImVec2 center(imagePos.x + (regionSize.x * 0.5F),
                      imagePos.y + (regionSize.y * 0.5F));
  const ImVec2 textPos(center.x - (textSize.x * 0.5F),
                       center.y - (blockHeight * 0.5F));
  const ImVec2 pad = style.WindowPadding;
  const float boxWidth = std::max(textSize.x, buttonSize.x);
  ImGui::GetWindowDrawList()->AddRectFilled(
      ImVec2(center.x - (boxWidth * 0.5F) - pad.x, textPos.y - pad.y),
      ImVec2(center.x + (boxWidth * 0.5F) + pad.x,
             textPos.y + blockHeight + pad.y),
      IM_COL32(20, 20, 20, 210), style.WindowRounding);
  ImGui::SetCursorScreenPos(textPos);
  // The wrap position is in window coordinates, not screen ones.
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textSize.x);
  ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.35F, 1.0F), "%s", notice);
  ImGui::PopTextWrapPos();
  if (!offerCreate) {
    return;
  }
  ImGui::SetCursorScreenPos(
      ImVec2(center.x - (buttonSize.x * 0.5F),
             textPos.y + textSize.y + style.ItemSpacing.y));
  ImGui::BeginDisabled(!world_is_editable());
  if (ImGui::Button(buttonLabel, buttonSize)) {
    EntityMenuChoice choice{};
    choice.kind = EntityMenuChoice::Kind::CreateCamera;
    static_cast<void>(run_entity_menu_choice(choice, EntitySpawnPlacement{}));
  }
  ImGui::EndDisabled();
}

} // namespace

void draw_game_view_panel() noexcept {
  // A layout saved before the Game view existed has no place for it: it
  // opens as a tab beside the Scene view instead of floating.
  const ImGuiWindow *sceneWindow = ImGui::FindWindowByName(kSceneViewWindow);
  if ((sceneWindow != nullptr) && (sceneWindow->DockId != 0U)) {
    ImGui::SetNextWindowDockID(sceneWindow->DockId, ImGuiCond_FirstUseEver);
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
  const bool visible = ImGui::Begin(kGameViewWindow);
  ImGui::PopStyleVar();

  EditorSession &session = editor_session();
  session.gameViewShown = visible;
  session.gameViewFocused = visible && ImGui::IsWindowFocused();
  session.gameViewHovered = visible && ImGui::IsWindowHovered();
  const bool capturing = begin_game_view_screenshot_frame(visible);
  if (!visible) {
    ImGui::End();
    return;
  }

  draw_game_view_toolbar(capturing);
  const ImVec2 regionSize = ImGui::GetContentRegionAvail();
  session.gameViewScreenPos = ImGui::GetCursorScreenPos();
  draw_game_view_menu(session.gameViewScreenPos.y, capturing);
  session.gameViewScreenSize = regionSize;
  int width = 0;
  int height = 0;
  region_pixels(regionSize, &width, &height);
  renderer::set_game_view_size(width, height);

  draw_view_image(renderer::RenderViewId::Game, regionSize);
  if (capturing) {
    // The part of the image the window shows: its border is drawn over
    // the image's edge, inside the window's rect but outside its clip.
    const ImRect clip = ImGui::GetCurrentWindow()->InnerClipRect;
    const ImVec2 imageMin = session.gameViewScreenPos;
    const ImVec2 visibleMin(std::max(imageMin.x, clip.Min.x),
                            std::max(imageMin.y, clip.Min.y));
    const ImVec2 visibleMax(std::min(imageMin.x + regionSize.x, clip.Max.x),
                            std::min(imageMin.y + regionSize.y, clip.Max.y));
    static_cast<void>(take_game_view_screenshot(game_view_screenshot_region(
        visibleMin,
        ImVec2(visibleMax.x - visibleMin.x, visibleMax.y - visibleMin.y),
        ImGui::GetMainViewport()->Pos,
        ImGui::GetIO().DisplayFramebufferScale)));
  }

  // What stops the game from rendering, over the image where it shows,
  // as Unity's "No cameras rendering"; nothing when one camera renders,
  // and nothing in the frame a screenshot is taken.
  char notice[192] = {};
  if (!capturing && (session.world != nullptr) &&
      game_camera_notice(*session.world, notice, sizeof(notice))) {
    draw_game_camera_notice(notice, !game_view_has_camera(*session.world),
                            session.gameViewScreenPos, regionSize);
  }
  ImGui::End();
}

bool editor_scene_view(renderer::RenderViewDesc *outView) noexcept {
  const EditorSession &session = editor_session();
  if ((outView == nullptr) || !session.sceneViewShown ||
      (session.sceneViewPixelWidth <= 0) ||
      (session.sceneViewPixelHeight <= 0)) {
    return false;
  }
  outView->id = renderer::RenderViewId::Scene;
  outView->camera = editor_camera_state(session.editorCamera);
  outView->width = session.sceneViewPixelWidth;
  outView->height = session.sceneViewPixelHeight;
  outView->drawScene = true;
  outView->drawOverlays = true;
  return true;
}

bool editor_game_view_visible() noexcept {
  return editor_session().gameViewShown;
}

} // namespace engine::editor
