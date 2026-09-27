// Declares the editor's Rendering panel: the renderer's author-facing
// settings (fog, exposure, post effects, shadows), opened from the Window
// menu.

#pragma once

namespace engine::editor {

/// Registers editor.show_rendering, the panel's visibility.
void register_rendering_panel_cvars() noexcept;

/// Draws the Rendering panel while editor.show_rendering is set.
void draw_rendering_panel() noexcept;

} // namespace engine::editor
