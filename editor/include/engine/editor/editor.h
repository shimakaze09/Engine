// Declares editor types and APIs for the Engine editor tool.

#pragma once

namespace engine::runtime {
/// Owns the world behavior and state.
class World;
}

namespace engine::editor {

/// Initializes the owning system for editor.
bool initialize_editor(void *sdlWindow, void *glContext) noexcept;
/// Shuts down the owning system for editor.
void shutdown_editor() noexcept;
/// Handles editor new frame.
void editor_new_frame() noexcept;
/// Handles editor render.
void editor_render(float frameMs, float utilizationPct) noexcept;
/// Handles editor process event.
void editor_process_event(void *sdlEvent) noexcept;
/// Handles editor set world.
void editor_set_world(runtime::World *world) noexcept;
/// Handles editor is playing.
bool editor_is_playing() noexcept;
/// Handles editor is paused.
bool editor_is_paused() noexcept;

} // namespace engine::editor
