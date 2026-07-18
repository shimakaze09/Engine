// Declares editor types and APIs for the Engine editor tool.

#pragma once

namespace engine::runtime {
class World;
}

namespace engine::editor {

/// Initializes the owning system for editor.
bool initialize_editor(void *sdlWindow, void *glContext) noexcept;
/// Shuts down the owning system for editor.
void shutdown_editor() noexcept;
/// Starts an ImGui frame (call before any panel draws).
void editor_new_frame() noexcept;
/// Draws all editor panels and renders the ImGui frame.
void editor_render(float frameMs, float utilizationPct) noexcept;
/// Feeds one SDL event to ImGui (and gizmo) input.
void editor_process_event(void *sdlEvent) noexcept;
/// Attaches or detaches the runtime world the editor edits.
void editor_set_world(runtime::World *world) noexcept;
/// True while play mode is active.
bool editor_is_playing() noexcept;
/// True while play mode is paused.
bool editor_is_paused() noexcept;

} // namespace engine::editor
