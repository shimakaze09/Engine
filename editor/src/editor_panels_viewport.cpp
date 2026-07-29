// Implements the editor scene viewport panel, gizmos, and collider overlay.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_viewport.h"

#include "editor_commands.h"
#include "editor_session.h"
#include "editor_transform_util.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include <SDL3/SDL_opengl.h>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl3.h"
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
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/command_history.h"
#include "engine/editor/debug_camera.h"

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

} // namespace

void draw_scene_viewport_panel() noexcept {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
  const bool visible = ImGui::Begin("Scene");
  ImGui::PopStyleVar();

  if (!visible) {
    ImGui::End();
    return;
  }

  const ImVec2 regionSize = ImGui::GetContentRegionAvail();
  const ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();

  editor_session().sceneViewportScreenPos = cursorScreenPos;
  editor_session().sceneViewportScreenSize = regionSize;

  renderer::set_scene_viewport_size(static_cast<int>(regionSize.x),
                                    static_cast<int>(regionSize.y));

  const std::uint32_t texId = renderer::get_scene_viewport_texture();
  if ((texId != 0U) && (regionSize.x > 0.0F) && (regionSize.y > 0.0F)) {
    ImGui::Image(static_cast<ImTextureID>(texId), regionSize,
                 ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
  } else {
    ImGui::TextUnformatted("Waiting for renderer...");
  }

  // --- ImGuizmo gizmo rendering ---
  const bool editable = world_is_editable();
  const runtime::Entity selectedEntity =
      (editor_session().world != nullptr &&
       editor_session().selectedEntityIndex != 0U)
          ? editor_session().world->find_entity_by_index(
                editor_session().selectedEntityIndex)
          : runtime::kInvalidEntity;

  const bool hasTransform = (selectedEntity != runtime::kInvalidEntity) &&
                            (editor_session().world != nullptr) &&
                            (editor_session().world->get_transform_read_ptr(
                                 selectedEntity) != nullptr);

  if (selectedEntity != runtime::kInvalidEntity) {
    draw_selected_collider_overlay(selectedEntity);
  }

  if (editable && hasTransform && (regionSize.x > 0.0F) &&
      (regionSize.y > 0.0F)) {
    const renderer::CameraState cam =
        editor_camera_state(editor_session().editorCamera);

    constexpr float kDefaultFov = 1.0471975512F;
    constexpr float kNear = 0.1F;
    constexpr float kFar = 100.0F;

    const float aspect = regionSize.x / regionSize.y;
    const math::Mat4 viewMat = math::look_at(cam.position, cam.target, cam.up);
    const math::Mat4 projMat =
        math::perspective(kDefaultFov, aspect, kNear, kFar);

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

    const bool manipulated = ImGuizmo::Manipulate(
        &viewMat.columns[0].x, &projMat.columns[0].x, editor_session().gizmoOp,
        ImGuizmo::LOCAL, &modelMat.columns[0].x);

    // Track gizmo drag start/end for undo.
    const bool gizmoUsing = ImGuizmo::IsUsing();
    if (gizmoUsing && !editor_session().gizmoWasUsing) {
      editor_session().gizmoStartTransform = transform;
    }

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

    if (!gizmoUsing && editor_session().gizmoWasUsing) {
      auto *cmd = new (std::nothrow) TransformEditCommand();
      if (cmd != nullptr) {
        cmd->entity = selectedEntity;
        cmd->oldTransform = editor_session().gizmoStartTransform;
        editor_session().world->get_transform(selectedEntity,
                                              &cmd->newTransform);
        editor_session().commandHistory.execute(cmd);
      }
    }
    editor_session().gizmoWasUsing = gizmoUsing;
  }

  // Camera input: only when stopped/paused, viewport hovered, gizmo not active.
  const bool debugDetach = core::cvar_get_bool("debug.camera_detach", false);
  if (debugDetach && !editor_session().debugCameraActive) {
    // Snapshot the current camera on transition to detached mode.
    if (editor_session().playState == PlayState::Playing) {
      editor_session().frozenCameraState = renderer::get_active_camera();
    } else {
      editor_session().frozenCameraState =
          editor_camera_state(editor_session().editorCamera);
    }
    editor_session().debugCamera.position =
        editor_session().frozenCameraState.position;
    editor_session().debugCameraActive = true;
  } else if (!debugDetach && editor_session().debugCameraActive) {
    editor_session().debugCameraActive = false;
  }

  if (editor_session().debugCameraActive && ImGui::IsWindowHovered()) {
    // Free-fly debug camera with WASD + mouse
    const ImGuiIO &io = ImGui::GetIO();
    const bool rmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const float dt = io.DeltaTime;
    update_debug_camera(
        editor_session().debugCamera, dt, ImGui::IsKeyDown(ImGuiKey_W),
        ImGui::IsKeyDown(ImGuiKey_S), ImGui::IsKeyDown(ImGuiKey_A),
        ImGui::IsKeyDown(ImGuiKey_D), ImGui::IsKeyDown(ImGuiKey_E),
        ImGui::IsKeyDown(ImGuiKey_Q), io.KeyShift,
        rmbDown ? static_cast<int>(io.MouseDelta.x) : 0,
        rmbDown ? static_cast<int>(io.MouseDelta.y) : 0);
    renderer::set_active_camera(
        debug_camera_state(editor_session().debugCamera));

    // Draw the frozen game camera frustum as wireframe.
    const float aspect =
        (regionSize.y > 0.0F) ? (regionSize.x / regionSize.y) : 1.0F;
    draw_camera_frustum_wireframe(editor_session().frozenCameraState, aspect);
  } else if ((editor_session().playState != PlayState::Playing) &&
             ImGui::IsWindowHovered() && !ImGuizmo::IsUsing()) {
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

  // Push editor camera when not playing (and debug camera is not active).
  if ((editor_session().playState != PlayState::Playing) &&
      !editor_session().debugCameraActive) {
    renderer::set_active_camera(
        editor_camera_state(editor_session().editorCamera));
  }

  ImGui::End();
}

} // namespace engine::editor
