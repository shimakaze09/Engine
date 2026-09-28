// Declares the editor Console panel: log view, filters, and the menu-bar
// status that shows unseen warnings and errors and opens the panel.

#pragma once

namespace engine::editor {

/// Draws the dockable Console panel (severity/search/channel/session
/// filters, collapse, pause/autoscroll, copy, clear, click-to-navigate).
/// A no-op except for marking entries seen when the window is collapsed.
void draw_console_panel() noexcept;

/// Width the menu-bar console status takes this frame; 0 when no warning
/// or error is unseen, since the status then draws nothing.
float console_status_indicator_width() noexcept;

/// Draws the menu-bar console status: nothing while no warning or error
/// has arrived since the Console was last visible, otherwise their counts
/// in the severity's color, which a click answers by showing the Console
/// and bringing it to the front, as Unity's status bar does.
void draw_console_status_indicator() noexcept;

} // namespace engine::editor
