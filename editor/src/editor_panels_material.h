// Declares the material editor panel (issue #160): scalar/vector params,
// alpha mode, UV transform, and texture-slot pickers for the currently
// open material asset, with undoable live viewport feedback.

#pragma once

namespace engine::editor {

/// Draws the dockable Material editor panel when open (see
/// open_material_editor in editor_material_edit.h); a no-op when closed.
void draw_material_editor_panel() noexcept;

} // namespace engine::editor
