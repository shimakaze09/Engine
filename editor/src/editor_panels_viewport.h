// Declares the editor's two viewport panels: the Scene view (the editor
// camera and overlays, during play too; gizmos while the world is
// editable) and the Game view (what the player sees; game input follows
// its focus), and the bridge hooks through which the pipeline renders both.

#pragma once

#include "engine/core/engine_stats.h"
#include "engine/renderer/command_buffer.h"

namespace engine::editor {

/// Draws the Scene view with ImGuizmo transform gizmos.
void draw_scene_viewport_panel() noexcept;
/// Draws the Game view: the Game render view's image at the panel's size.
void draw_game_view_panel() noexcept;

/// The Scene view the pipeline renders next frame: the editor camera at
/// the Scene panel's pixel size; false while the panel is hidden.
bool editor_scene_view(renderer::RenderViewDesc *outView) noexcept;
/// Whether the Game panel was shown last frame.
bool editor_game_view_visible() noexcept;

} // namespace engine::editor
