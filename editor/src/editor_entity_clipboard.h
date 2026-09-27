// Declares the editor's entity clipboard: Copy captures the selection as
// a forest of subtrees (every persistent component, the roots' world
// poses), and Paste or Paste As Child creates fresh copies of it through
// one undoable duplicate command, as Unity's Copy, Paste and Paste As
// Child do.

#pragma once

#include "engine/runtime/world.h"

namespace engine::editor {

/// Copies the selection into the clipboard, replacing what it held. Reads
/// only, so it works during play and the copy can be pasted after Stop.
/// False when nothing is selected or the capture cannot be allocated.
bool entity_clipboard_copy() noexcept;
/// True when the clipboard holds entities.
bool entity_clipboard_has() noexcept;
/// Empties the clipboard.
void entity_clipboard_clear() noexcept;

/// Pastes fresh copies of the clipboard as one undoable command and
/// selects the pasted roots.
///
/// Each root is placed as follows:
/// - With `parent` valid (Paste As Child), under `parent`, keeping the
///   world pose it was copied with.
/// - Otherwise, beside its original, under the same parent, when that
///   parent still exists in this document.
/// - Otherwise, at the scene root with its copied world pose.
///
/// References into the pasted set point at the new copies. A reference
/// out of it is kept within the document it was copied in, and cleared in
/// another. False, with the world untouched, when the world is not
/// editable, the clipboard is empty, or the paste is refused.
bool execute_entity_paste(runtime::Entity parent) noexcept;

} // namespace engine::editor
