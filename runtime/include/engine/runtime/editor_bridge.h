// Declares editor bridge types and APIs for the Engine runtime world.

#pragma once

#include <cstdint>

namespace engine::runtime {

class World;
struct EngineAssetDatabaseService;

/// Function-pointer bridge the runtime uses to reach the editor. Initialize,
/// shutdown, new-frame, and render callbacks run with the render context
/// current.
struct EditorBridge final {
  bool (*initialize)(void *sdlWindow, void *glContext) noexcept = nullptr;
  void (*shutdown)() noexcept = nullptr;
  void (*new_frame)() noexcept = nullptr;
  void (*render)(float frameMs, float utilizationPct) noexcept = nullptr;
  void (*process_event)(void *sdlEvent) noexcept = nullptr;
  void (*set_world)(World *world) noexcept = nullptr;
  bool (*is_playing)() noexcept = nullptr;
  bool (*is_paused)() noexcept = nullptr;
  bool (*wants_capture_keyboard)() noexcept = nullptr;
  bool (*wants_capture_mouse)() noexcept = nullptr;
  // True at most once per Step click while paused: the pipeline consumes
  // the request and simulates exactly one fixed step that frame.
  bool (*consume_step_request)() noexcept = nullptr;
};

/// Sets the requested value for editor bridge.
void set_editor_bridge(const EditorBridge *bridge) noexcept;
/// Registered bridge instance, or nullptr when no editor is linked.
const EditorBridge *editor_bridge() noexcept;

/// Publishes (or clears, with nullptr) the pipeline's asset service so
/// editor authoring can request asset loads while the runtime is active.
void set_editor_asset_service(EngineAssetDatabaseService *service) noexcept;
/// Requests an async mesh load for an editor-authored reference by virtual
/// path and returns the path-derived asset id; 0 when no runtime asset
/// service is published, the path cannot resolve, or the request fails.
std::uint64_t editor_request_mesh_asset(const char *virtualPath) noexcept;

} // namespace engine::runtime
