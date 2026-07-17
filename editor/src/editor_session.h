// Declares the private editor session state (attached world, selection,
// play/gizmo/camera state, thumbnail cache) plus session lifecycle helpers
// shared by the editor panel translation units. Split out of editor.cpp
// (REVIEW_FINDINGS A3).

#pragma once

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
  PlayState playState = PlayState::Stopped;
  std::unique_ptr<char[]> playSnapshotBuffer{};
  std::size_t playSnapshotCapacity = 0U;
  std::size_t playSnapshotSize = 0U;
  bool hasPlaySnapshot = false;
  bool worldRestoreFailed = false;
  EditorCamera editorCamera{};
  ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;
  bool gizmoWasUsing = false;
  runtime::Transform gizmoStartTransform{};
  CommandHistory commandHistory{};
  DebugCamera debugCamera{};
  renderer::CameraState frozenCameraState{};
  bool debugCameraActive = false;
  char selectedAssetPath[512] = {};
  std::array<ThumbnailEntry, kMaxThumbnails> thumbnailCache{};
  std::size_t thumbnailCount = 0U;
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
constexpr const char *kScriptSectionLabel = "ScriptComponent";

/// Returns the process-wide editor session state.
EditorSession &editor_session() noexcept;

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
