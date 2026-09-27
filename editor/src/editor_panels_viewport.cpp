// Implements the editor's Scene and Game view panels, the Scene view's
// gizmos and overlays, and the bridge hooks that hand both views to the
// pipeline.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_viewport.h"

#include "editor_commands.h"
#include "editor_session.h"
#include "editor_transform_util.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

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
  const math::Vec4 worldFrom =
      math::mul(localToWorld, math::Vec4(from.x, from.y, from.z, 1.0F));
  const math::Vec4 worldTo =
      math::mul(localToWorld, math::Vec4(to.x, to.y, to.z, 1.0F));
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

  const ImVec2 mouse = ImGui::GetMousePos();
  const float ndcX = (((mouse.x - imagePos.x) / imageSize.x) * 2.0F) - 1.0F;
  const float ndcY = 1.0F - (((mouse.y - imagePos.y) / imageSize.y) * 2.0F);
  const float aspect = imageSize.x / imageSize.y;
  math::Ray ray{};
  if (!viewport_ray(math::look_at(cam.position, cam.target, cam.up),
                    renderer::camera_projection_matrix(cam, aspect),
                    renderer::device_depth_zero_one(), ndcX, ndcY, &ray) ||
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
  } else {
    ImGui::TextUnformatted("Waiting for renderer...");
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

  if (selectedEntity != runtime::kInvalidEntity) {
    draw_selected_collider_overlay(selectedEntity);
    if (regionSize.y > 0.0F) {
      draw_selected_camera_frustum_overlay(selectedEntity,
                                           regionSize.x / regionSize.y);
    }
  }

  if (editable && hasTransform && (regionSize.x > 0.0F) &&
      (regionSize.y > 0.0F)) {
    const renderer::CameraState cam =
        editor_camera_state(editor_session().editorCamera);

    const float aspect = regionSize.x / regionSize.y;
    const math::Mat4 viewMat = math::look_at(cam.position, cam.target, cam.up);
    // The renderer's own builder, so the gizmo projects with the camera
    // the Scene view renders. ImGuizmo reads only x and y and unprojects
    // its picking ray between clip depths 0 and 1, which lie on the view
    // ray under either depth convention.
    const math::Mat4 projMat = renderer::camera_projection_matrix(cam, aspect);

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

  // The editor camera flies whenever the Scene view is hovered and no
  // gizmo drag holds the mouse, during play as well: the Scene view is the
  // author's, and the game has its own view.
  if (ImGui::IsWindowHovered() && !ImGuizmo::IsUsing()) {
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
  if (!visible) {
    ImGui::End();
    return;
  }

  const ImVec2 regionSize = ImGui::GetContentRegionAvail();
  session.gameViewScreenPos = ImGui::GetCursorScreenPos();
  session.gameViewScreenSize = regionSize;
  int width = 0;
  int height = 0;
  region_pixels(regionSize, &width, &height);
  renderer::set_game_view_size(width, height);

  draw_view_image(renderer::RenderViewId::Game, regionSize);
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
