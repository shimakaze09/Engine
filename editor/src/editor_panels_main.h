// Declares the editor main menu bar, play toolbar, and entity hierarchy panel.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include "engine/core/engine_stats.h"

namespace engine::editor {

/// Draws the scene/edit/panels menu bar.
void draw_main_menu_bar() noexcept;
/// Draws the play/pause/stop toolbar.
void draw_toolbar() noexcept;
/// Draws the entity hierarchy panel.
void draw_entities_panel() noexcept;

} // namespace engine::editor
