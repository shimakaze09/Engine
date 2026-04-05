#pragma once

namespace engine::runtime {

class World;

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
};

void set_editor_bridge(const EditorBridge *bridge) noexcept;
const EditorBridge *editor_bridge() noexcept;

} // namespace engine::runtime
