// Declares the editor's own preferences (the CJK font file, the main
// window's geometry, the gizmo's axes, the Scene grid, the autosave
// interval and rebound shortcuts) and the Preferences window that edits
// them. They persist in the editor's layout file, through an ImGui
// settings section, so they are staged and replaced atomically with the
// layout rather than kept in a file of their own.

#pragma once

#include <cstddef>

namespace engine::editor {

/// Registers the preference cvars and the settings section that carries
/// them. Must run after the ImGui context exists and before the layout
/// loads, so a stored preference is applied before the fonts are built.
void register_editor_preferences() noexcept;

/// Reopens the main window at the geometry the layout file stored, once,
/// after the layout has loaded; a first launch, with nothing stored, keeps
/// the display-scaled default the platform opened.
void apply_stored_window_geometry() noexcept;

/// Records the CJK font the editor loaded at startup, for display.
void set_loaded_cjk_font(const char *path) noexcept;

/// Draws the Preferences window while editor.show_preferences is set.
void draw_editor_preferences_panel() noexcept;

/// The preferences section as ImGui writes it into the layout file, into
/// `out` (for tests). Returns the characters written, 0 when it does not
/// fit.
std::size_t editor_preferences_section(char *out,
                                       std::size_t capacity) noexcept;

} // namespace engine::editor
