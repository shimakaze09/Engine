// Declares the editor Console panel (issue #155): log view, filters, and
// the non-spamming status indicator shown while the panel is closed.

#pragma once

namespace engine::editor {

/// Draws the dockable Console panel (severity/search/channel/session
/// filters, collapse, pause/autoscroll, copy, clear, click-to-navigate).
/// A no-op except for marking entries seen when the window is collapsed.
void draw_console_panel() noexcept;

/// Draws a small non-spamming badge (error/warning counts since the panel
/// was last visible) into the current ImGui context — used by the main
/// menu bar so Fatal/high-severity errors stay visible even when the
/// Console panel itself is closed.
void draw_console_status_indicator() noexcept;

} // namespace engine::editor
