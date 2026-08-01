// Declares the private editor session state (attached world, selection,
// play/gizmo/camera state, thumbnail cache) plus session lifecycle helpers
// shared by the editor panel translation units. Split out of editor.cpp
// (REVIEW_FINDINGS A3).

#pragma once

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include <SDL3/SDL_opengl.h>

#include "imgui.h"
#include "ImGuizmo.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "engine/editor/command_history.h"
#include "engine/editor/debug_camera.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// Enumerates play state values used by the engine.
enum class PlayState : std::uint8_t { Stopped, Playing, Paused };

// Thumbnail cache: maps a file path to a GL texture.
struct ThumbnailEntry final {
  char path[512] = {};
  GLuint textureId = 0U;
  int width = 0;
  int height = 0;
};
constexpr std::size_t kMaxThumbnails = 128U;

/// Owns editor UI/session state for the currently attached runtime world.
struct EditorSession final {
  bool initialized = false;
  runtime::World *world = nullptr;
  std::uint32_t selectedEntityIndex = 0U;
  static constexpr std::size_t kMaxSelectedEntities = 64U;
  std::array<std::uint32_t, kMaxSelectedEntities> selectedEntities{};
  std::size_t selectedEntityCount = 0U;
  PlayState playState = PlayState::Stopped;
  // Set by the toolbar Step button while paused; the runtime consumes it
  // through the editor bridge to simulate exactly one fixed step.
  bool stepRequested = false;
  std::unique_ptr<char[]> playSnapshotBuffer{};
  std::size_t playSnapshotCapacity = 0U;
  std::size_t playSnapshotSize = 0U;
  bool hasPlaySnapshot = false;
  // World the Play snapshot was captured from; Stop refuses to restore
  // into any other world.
  const runtime::World *playSnapshotWorld = nullptr;
  bool worldRestoreFailed = false;
  EditorCamera editorCamera{};
  ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;
  bool gizmoWasUsing = false;
  runtime::Transform gizmoStartTransform{};
  bool snapEnabled = false;
  float snapStep = 0.5F;
  float snapAngleDegrees = 15.0F;
  CommandHistory commandHistory{};
  DebugCamera debugCamera{};
  renderer::CameraState frozenCameraState{};
  bool debugCameraActive = false;
  char selectedAssetPath[512] = {};
  std::array<ThumbnailEntry, kMaxThumbnails> thumbnailCache{};
  std::size_t thumbnailCount = 0U;
  // Screen rect of the Scene panel's image, recorded each frame by the
  // viewport panel so overlays can anchor inside the rendered scene.
  ImVec2 sceneViewportScreenPos{};
  ImVec2 sceneViewportScreenSize{};
};

constexpr const char *kTransformTypeName = "engine::runtime::Transform";
constexpr const char *kRigidBodyTypeName = "engine::runtime::RigidBody";
constexpr const char *kColliderTypeName = "engine::runtime::Collider";
constexpr const char *kNameTypeName = "engine::runtime::NameComponent";
constexpr const char *kReflectionProbeTypeName =
    "engine::runtime::ReflectionProbeComponent";
constexpr const char *kFoliagePatchTypeName =
    "engine::runtime::FoliagePatchComponent";
constexpr const char *kPointLightTypeName =
    "engine::runtime::PointLightComponent";
constexpr const char *kSpotLightTypeName =
    "engine::runtime::SpotLightComponent";
constexpr const char *kSpringArmTypeName =
    "engine::runtime::SpringArmComponent";
constexpr const char *kSceneCaptureTypeName =
    "engine::runtime::SceneCaptureComponent";
constexpr const char *kTransformSectionLabel = "Transform";
constexpr const char *kRigidBodySectionLabel = "RigidBody";
constexpr const char *kColliderSectionLabel = "Collider";
constexpr const char *kMeshSectionLabel = "MeshComponent";
constexpr const char *kFoliagePatchSectionLabel = "FoliagePatchComponent";
constexpr const char *kLightSectionLabel = "LightComponent";
constexpr const char *kReflectionProbeSectionLabel = "ReflectionProbeComponent";
constexpr const char *kPointLightSectionLabel = "PointLightComponent";
constexpr const char *kSpotLightSectionLabel = "SpotLightComponent";
constexpr const char *kSpringArmSectionLabel = "SpringArmComponent";
constexpr const char *kSceneCaptureSectionLabel = "SceneCaptureComponent";
constexpr const char *kScriptSectionLabel = "ScriptComponent";
constexpr const char *kAnimationSectionLabel = "AnimationComponent";

/// Returns the process-wide editor session state.
EditorSession &editor_session() noexcept;

/// True when the entity index is in the multi-selection.
bool is_entity_selected(std::uint32_t entityIndex) noexcept;
/// Selects an entity: replaces the selection, or toggles membership when
/// additive (Ctrl-click). The primary selection follows the last pick.
void select_entity(std::uint32_t entityIndex, bool additive) noexcept;
/// Clears the multi-selection and the primary selection.
void clear_entity_selection() noexcept;

/// Returns the configured editor scene path ("" when unset).
const char *editor_scene_path() noexcept;
/// Returns the configured editor asset browser root ("" when unset).
const char *editor_asset_root() noexcept;

/// Loads (and caches) the thumbnail texture for an asset path; 0 on miss.
GLuint load_thumbnail_texture(const char *assetPath) noexcept;
/// Releases cached thumbnail GL textures owned by the editor.
void clear_thumbnail_cache() noexcept;

/// True when the attached world exists, is stopped, and accepts edits.
bool world_is_editable() noexcept;
/// True when the attached world can run a scene load right now.
bool world_can_load_scene() noexcept;
/// Returns whether the default scene file is available on disk.
bool default_scene_file_exists() noexcept;

/// Serializes the current world so Stop can restore the pre-play state.
bool capture_play_snapshot() noexcept;
/// Enters play mode (captures the play snapshot first).
void start_play_mode() noexcept;
/// Toggles between Playing and Paused.
void pause_play_mode() noexcept;
/// Stops play mode and restores the captured pre-play world.
void stop_play_mode() noexcept;

} // namespace engine::editor
