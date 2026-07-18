// Implements the editor scene viewport panel, gizmos, and collider overlay.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_viewport.h"

#include "editor_commands.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#if __has_include(<SDL_opengl.h>)
#include <SDL_opengl.h>
#elif __has_include(<SDL2/SDL_opengl.h>)
#include <SDL2/SDL_opengl.h>
#else
#error "SDL OpenGL headers not found"
#endif

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/engine.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/command_history.h"
#include "engine/editor/debug_camera.h"

#include <stb_image.h>

namespace engine::editor {

namespace {

bool project_world_to_screen(const math::Vec3 &worldPos, const math::Mat4 &vp,
                             const ImVec2 &viewportOrigin,
                             const ImVec2 &viewportSize,
                             ImVec2 *outScreen) noexcept {
  if ((outScreen == nullptr) || (viewportSize.x <= 0.0F) ||
      (viewportSize.y <= 0.0F)) {
    return false;
  }

  const math::Vec4 clip =
      math::mul(vp, math::Vec4(worldPos.x, worldPos.y, worldPos.z, 1.0F));
  if (clip.w <= 0.0001F) {
    return false;
  }

  const float invW = 1.0F / clip.w;
  const float ndcX = clip.x * invW;
  const float ndcY = clip.y * invW;

  outScreen->x = viewportOrigin.x + ((ndcX * 0.5F) + 0.5F) * viewportSize.x;
  outScreen->y =
      viewportOrigin.y + (1.0F - ((ndcY * 0.5F) + 0.5F)) * viewportSize.y;
  return true;
}

void draw_selected_collider_overlay(const runtime::Entity selectedEntity,
                                    const math::Mat4 &viewProjection,
                                    const ImVec2 &viewportOrigin,
                                    const ImVec2 &viewportSize) noexcept {
  if ((editor_session().world == nullptr) || (selectedEntity == runtime::kInvalidEntity)) {
    return;
  }

  const runtime::Collider *collider = editor_session().world->get_collider_ptr(selectedEntity);
  if (collider == nullptr) {
    return;
  }

  const runtime::WorldTransform *worldTransform =
      editor_session().world->get_world_transform_read_ptr(selectedEntity);
  if (worldTransform == nullptr) {
    return;
  }

  math::Vec3 halfExtents = collider->halfExtents;
  if (collider->shape == runtime::ColliderShape::Sphere) {
    const float r = collider->halfExtents.x;
    halfExtents = math::Vec3(r, r, r);
  }

  const math::Vec3 c = worldTransform->position;
  const math::Vec3 corners[8] = {
      math::Vec3(c.x - halfExtents.x, c.y - halfExtents.y, c.z - halfExtents.z),
      math::Vec3(c.x + halfExtents.x, c.y - halfExtents.y, c.z - halfExtents.z),
      math::Vec3(c.x + halfExtents.x, c.y + halfExtents.y, c.z - halfExtents.z),
      math::Vec3(c.x - halfExtents.x, c.y + halfExtents.y, c.z - halfExtents.z),
      math::Vec3(c.x - halfExtents.x, c.y - halfExtents.y, c.z + halfExtents.z),
      math::Vec3(c.x + halfExtents.x, c.y - halfExtents.y, c.z + halfExtents.z),
      math::Vec3(c.x + halfExtents.x, c.y + halfExtents.y, c.z + halfExtents.z),
      math::Vec3(c.x - halfExtents.x, c.y + halfExtents.y, c.z + halfExtents.z),
  };

  ImVec2 projected[8] = {};
  bool visible[8] = {};
  for (int i = 0; i < 8; ++i) {
    visible[i] =
        project_world_to_screen(corners[i], viewProjection, viewportOrigin,
                                viewportSize, &projected[i]);
  }

  static constexpr int kEdges[24] = {
      0, 1, 1, 2, 2, 3, 3, 0, // back face
      4, 5, 5, 6, 6, 7, 7, 4, // front face
      0, 4, 1, 5, 2, 6, 3, 7  // side links
  };

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const ImU32 color = IM_COL32(40, 255, 120, 220);
  for (int i = 0; i < 24; i += 2) {
    const int a = kEdges[i];
    const int b = kEdges[i + 1];
    if (visible[a] && visible[b]) {
      drawList->AddLine(projected[a], projected[b], color, 2.0F);
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
      (editor_session().world != nullptr && editor_session().selectedEntityIndex != 0U)
          ? editor_session().world->find_entity_by_index(editor_session().selectedEntityIndex)
          : runtime::kInvalidEntity;

  const bool hasTransform =
      (selectedEntity != runtime::kInvalidEntity) && (editor_session().world != nullptr) &&
      (editor_session().world->get_transform_read_ptr(selectedEntity) != nullptr);

  if ((selectedEntity != runtime::kInvalidEntity) && (regionSize.x > 0.0F) &&
      (regionSize.y > 0.0F)) {
    const renderer::CameraState cam = (editor_session().playState == PlayState::Playing)
                                          ? renderer::get_active_camera()
                                          : editor_camera_state(editor_session().editorCamera);

    constexpr float kDefaultFov = 1.0471975512F;
    constexpr float kNear = 0.1F;
    constexpr float kFar = 100.0F;
    const float aspect = regionSize.x / regionSize.y;
    const math::Mat4 viewMat = math::look_at(cam.position, cam.target, cam.up);
    const math::Mat4 projMat =
        math::perspective(kDefaultFov, aspect, kNear, kFar);
    const math::Mat4 vp = math::mul(projMat, viewMat);
    draw_selected_collider_overlay(selectedEntity, vp, cursorScreenPos,
                                   regionSize);
  }

  if (editable && hasTransform && (regionSize.x > 0.0F) &&
      (regionSize.y > 0.0F)) {
    const renderer::CameraState cam = editor_camera_state(editor_session().editorCamera);

    constexpr float kDefaultFov = 1.0471975512F;
    constexpr float kNear = 0.1F;
    constexpr float kFar = 100.0F;

    const float aspect = regionSize.x / regionSize.y;
    const math::Mat4 viewMat = math::look_at(cam.position, cam.target, cam.up);
    const math::Mat4 projMat =
        math::perspective(kDefaultFov, aspect, kNear, kFar);

    runtime::Transform transform{};
    editor_session().world->get_transform(selectedEntity, &transform);
    math::Mat4 modelMat = math::compose_trs(
        transform.position, transform.rotation, transform.scale);

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
      math::Vec3 newPos{};
      math::Quat newRot{};
      math::Vec3 newScale{};
      if (math::decompose_trs(modelMat, &newPos, &newRot, &newScale)) {
        transform.position = newPos;
        transform.rotation = newRot;
        transform.scale = newScale;
        static_cast<void>(editor_session().world->add_transform(selectedEntity, transform));
      }
    }

    if (!gizmoUsing && editor_session().gizmoWasUsing) {
      auto *cmd = new (std::nothrow) TransformEditCommand();
      if (cmd != nullptr) {
        cmd->entity = selectedEntity;
        cmd->oldTransform = editor_session().gizmoStartTransform;
        editor_session().world->get_transform(selectedEntity, &cmd->newTransform);
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
      editor_session().frozenCameraState = editor_camera_state(editor_session().editorCamera);
    }
    editor_session().debugCamera.position = editor_session().frozenCameraState.position;
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
    renderer::set_active_camera(debug_camera_state(editor_session().debugCamera));

    // Draw the frozen game camera frustum as wireframe.
    const float aspect =
        (regionSize.y > 0.0F) ? (regionSize.x / regionSize.y) : 1.0F;
    draw_camera_frustum_wireframe(editor_session().frozenCameraState, aspect);
  } else if ((editor_session().playState != PlayState::Playing) && ImGui::IsWindowHovered() &&
             !ImGuizmo::IsUsing()) {
    const ImGuiIO &io = ImGui::GetIO();
    const bool altHeld = io.KeyAlt;
    const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    const int scrollDelta =
        (io.MouseWheel > 0.0F) ? 1 : ((io.MouseWheel < 0.0F) ? -1 : 0);

    update_editor_camera(editor_session().editorCamera, static_cast<int>(io.MouseDelta.x),
                         static_cast<int>(io.MouseDelta.y), scrollDelta,
                         altHeld && lmbDown, altHeld && mmbDown);
  }

  // Push editor camera when not playing (and debug camera is not active).
  if ((editor_session().playState != PlayState::Playing) && !editor_session().debugCameraActive) {
    renderer::set_active_camera(editor_camera_state(editor_session().editorCamera));
  }

  ImGui::End();
}


} // namespace engine::editor
