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

/// Places the next button beside the last item when it fits within the
/// window's content width, and otherwise leaves it to start the next line,
/// so a row of buttons wraps in a narrow panel instead of clipping.
void same_line_if_button_fits(const char *nextButtonLabel) noexcept;

} // namespace engine::editor
