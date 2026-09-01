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
#include "engine/renderer/render_device.h"
#include "engine/runtime/world.h"

#include "editor_asset_index.h"
#include "editor_scene_document.h"

namespace engine::editor {

/// Enumerates play state values used by the engine.
enum class PlayState : std::uint8_t { Stopped, Playing, Paused };

// Thumbnail cache: maps a file path to a device texture handle owned by
// the editor (created/destroyed through the engine-facing RenderDevice
// contract; the editor never touches GL types directly, per audit #206).
struct ThumbnailEntry final {
  char path[512] = {};
  renderer::DeviceTextureHandle texture{};
  int width = 0;
  int height = 0;
};
constexpr std::size_t kMaxThumbnails = 128U;

/// Bounded folder-navigation history for the content browser's back/forward
/// controls; in-session only (browser folder/filter identity itself is
/// what survives restart, through content_browser_state_load_once).
struct ContentBrowserNavHistory final {
  static constexpr std::size_t kMaxEntries = 32U;
  char entries[kMaxEntries][kMaxAssetIndexPath] = {};
  std::size_t count = 0U;
  std::size_t position = 0U;
};

/// Content-browser panel state: the active search/filter/folder request,
/// its change-driven match cache, and navigation history. Embedded in
/// EditorSession so panel draw code never owns per-frame filter state.
struct ContentBrowserState final {
  AssetFilterState filter{};
  AssetFilterCache filterCache{};
  AssetChildFolderCache childFolderCache{};
  ContentBrowserNavHistory navHistory{};
  bool persistedStateLoaded = false;
};

/// Owns editor UI/session state for the currently attached runtime world.
/// Selection stores full generation-checked entity handles plus the world
/// content epoch they were captured under: scene loads commit by whole-world
/// assignment, which resets generations, so an epoch mismatch invalidates
/// every retained handle even when index+generation would appear to match.
struct EditorSession final {
  bool initialized = false;
  runtime::World *world = nullptr;
  runtime::Entity selectedEntity{};
  static constexpr std::size_t kMaxSelectedEntities = 64U;
  std::array<runtime::Entity, kMaxSelectedEntities> selectedEntities{};
  std::size_t selectedEntityCount = 0U;
  std::uint32_t selectionEpoch = 0U;
  PlayState playState = PlayState::Stopped;
  // Set by the toolbar Step button while paused; the runtime consumes it
  // through the editor bridge to simulate exactly one fixed step.
  bool stepRequested = false;
  // Explicit opt-in (issue #159): while Playing/Paused, on lets the
  // Inspector write straight to the running world through editor_live_edit
  // instead of showing runtime values read-only. Never implies undo --
  // live edits stay outside command history regardless of this flag.
  bool liveEditEnabled = false;
  std::unique_ptr<char[]> playSnapshotBuffer{};
  std::size_t playSnapshotCapacity = 0U;
  std::size_t playSnapshotSize = 0U;
  bool hasPlaySnapshot = false;
  // World the Play snapshot was captured from; Stop refuses to restore
  // into any other world.
  const runtime::World *playSnapshotWorld = nullptr;
  bool worldRestoreFailed = false;
  // One-shot ENGINE_EDITOR_AUTOPLAY latch (#249): session-scoped so a
  // second editor session in one process autoplays again.
  bool autoplayConsumed = false;
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
  // Native window handle, retained for title-bar updates and as the
  // parent window for native file dialogs; never touched by Play/Stop.
  SDL_Window *sdlWindow = nullptr;
  char lastAppliedWindowTitle[640] = {};
  SceneDocumentState document{};
  ContentBrowserState contentBrowser{};
};

constexpr const char *kTransformTypeName = "engine::runtime::Transform";
constexpr const char *kRigidBodyTypeName = "engine::runtime::RigidBody";
constexpr const char *kColliderTypeName = "engine::runtime::Collider";
constexpr const char *kNameTypeName = "engine::runtime::NameComponent";
constexpr const char *kLightTypeName = "engine::runtime::LightComponent";
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
constexpr const char *kCameraTypeName = "engine::runtime::CameraComponent";
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

/// True when the exact entity handle is in the multi-selection and the
/// selection still belongs to the attached world's current content epoch.
bool is_entity_selected(runtime::Entity entity) noexcept;
/// Selects an entity: replaces the selection, or toggles membership when
/// additive (Ctrl-click). The primary selection follows the last pick.
void select_entity(runtime::Entity entity, bool additive) noexcept;
/// Clears the multi-selection and the primary selection.
void clear_entity_selection() noexcept;
/// Returns the validated primary selection: kInvalidEntity (after clearing
/// the stale state) unless the handle is alive in the attached world under
/// the epoch the selection was captured in.
runtime::Entity selected_entity() noexcept;
/// Drops selection entries whose entity died or whose world content epoch
/// changed (scene load / world restore reset generations).
void prune_entity_selection() noexcept;
/// Undoes the last command only while the world accepts edits (never
/// during play, after a failed restore, or outside the Input phase).
void editor_history_undo() noexcept;
/// Redoes the last undone command under the same editability gate.
void editor_history_redo() noexcept;

/// Returns the configured editor scene path ("" when unset).
const char *editor_scene_path() noexcept;
/// Returns the configured editor asset browser root ("" when unset).
const char *editor_asset_root() noexcept;

/// Loads (and caches) the thumbnail texture for an asset path through the
/// renderer's RenderDevice; invalid handle on miss.
renderer::DeviceTextureHandle
load_thumbnail_texture(const char *assetPath) noexcept;
/// Releases cached thumbnail textures owned by the editor through the
/// renderer's RenderDevice.
void clear_thumbnail_cache() noexcept;
/// ImGui image id for a device texture. The editor's ImGui backend renders
/// with the same graphics device, so this is the one sanctioned use of the
/// device's native texture id; 0 when the handle is stale or no device.
std::uint64_t imgui_texture_id(renderer::DeviceTextureHandle texture) noexcept;

/// Loads the persisted content-browser folder/filter (last-used folder and
/// type mask) once per process; no-op after the first call or when no
/// state was ever saved (defaults stand: index root, every type shown).
void content_browser_state_load_once() noexcept;
/// Persists the current folder + type mask to the platform save directory
/// (or the test override directory, when set).
void content_browser_state_persist() noexcept;
/// Test-only override for the content-browser state persistence directory;
/// an empty string restores the default per-user platform save directory.
void content_browser_state_set_directory_override_for_tests(
    const char *directory) noexcept;

/// Test-only switch: when set, the next initialize_editor call fails at
/// the ImGui backend step — after console capture and the other early
/// resources are acquired — so headless tests can drive the acquisition
/// rollback a real windowed backend failure reaches.
void editor_set_initialize_failure_for_tests(bool fail) noexcept;

/// Navigates the content browser to `folder` ("" = index root), recording
/// history so back/forward can retrace it, and persists the new folder. A
/// no-op when `folder` is already the current folder.
void content_browser_navigate(const char *folder) noexcept;
/// True when a back-navigation step is available.
bool content_browser_can_go_back() noexcept;
/// True when a forward-navigation step is available.
bool content_browser_can_go_forward() noexcept;
/// Steps the navigation history back one folder; no-op when unavailable.
void content_browser_go_back() noexcept;
/// Steps the navigation history forward one folder; no-op when
/// unavailable.
void content_browser_go_forward() noexcept;

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
