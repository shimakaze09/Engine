// Declares the Inspector's per-component operations: Reset to the value
// Add Component gives, and a one-slot component clipboard (Copy, Paste
// Component Values, Paste Component As New), as in Unity's component menu,
// and the menu that offers them. Each change to the world is one undoable
// command over every target.

#pragma once

#include <cstddef>

#include "editor_component_registry.h"

namespace engine::editor {

/// True when `type` can be reset. Name and Script are their entity's
/// identity rather than values (a generated name, a script path), so
/// neither has a Reset.
bool component_reset_available(ComponentEditType type) noexcept;

/// Resets `type` on every target that carries it to the value Add
/// Component gives it, as one undoable command. A Transform keeps its
/// parent: the hierarchy is not a component value. False when no target
/// changes.
bool execute_component_reset(const runtime::Entity *targets, std::size_t count,
                             ComponentEditType type) noexcept;

/// Copies `source`'s `type` component into the clipboard, replacing what
/// it held. Reads only, so it works during play. False when `source` does
/// not carry the component.
bool component_clipboard_copy(runtime::Entity source,
                              ComponentEditType type) noexcept;
/// True when the clipboard holds a component; its type goes to *outType.
bool component_clipboard_type(ComponentEditType *outType) noexcept;
/// Empties the clipboard.
void component_clipboard_clear() noexcept;

/// Writes the clipboard's values onto every target that carries a
/// component of its type, as one undoable command. What only the source
/// could own is not copied: a Transform keeps the target's parent, an
/// Animation starts from its controller's entry state, and an entity
/// reference copied from another scene document is cleared. False when no
/// target changes.
bool execute_component_paste_values(const runtime::Entity *targets,
                                    std::size_t count) noexcept;
/// Adds the clipboard's component to every target that lacks one of its
/// type, with the same rules, as one undoable command. False when every
/// target already has it.
bool execute_component_paste_as_new(const runtime::Entity *targets,
                                    std::size_t count) noexcept;

/// Draws the component menu on the item just drawn (a component header),
/// opened by right-click: Reset, Copy Component, Paste Component Values
/// and Paste Component As New over `targets`, the one entity or the
/// selection the section edits. Changes need `editable`; Copy does not.
void draw_component_menu(const runtime::Entity *targets, std::size_t count,
                         ComponentEditType type, bool editable) noexcept;

} // namespace engine::editor
