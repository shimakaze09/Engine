#pragma once

namespace engine::runtime {
class World;
}

namespace engine::editor {

bool initialize_editor(void *sdlWindow, void *glContext) noexcept;
void shutdown_editor() noexcept;
void editor_new_frame() noexcept;
void editor_render(float frameMs, float utilizationPct) noexcept;
void editor_process_event(void *sdlEvent) noexcept;
void editor_set_world(runtime::World *world) noexcept;
bool editor_is_playing() noexcept;
bool editor_is_paused() noexcept;

} // namespace engine::editor
