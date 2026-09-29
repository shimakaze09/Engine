// Declares renaming an entity in place, as Unity's F2 in the Hierarchy
// does: the Entities panel row becomes a text field holding the entity's
// name; Enter or clicking away commits it as one undoable edit, and Escape
// (or an empty name) leaves the name as it was.

#pragma once

#include "engine/runtime/world.h"

namespace engine::editor {

/// The entity being renamed and the text typed so far.
struct EntityRenameState final {
  /// Persistent id of the entity being renamed; invalid while none is.
  runtime::PersistentId target = runtime::kInvalidPersistentId;
  char buffer[runtime::NameComponent::kMaxNameLength + 1U] = {};
  /// Set when renaming begins, so the field takes keyboard focus once.
  bool focusPending = false;
};

/// Starts renaming `entity` with its current name in the field. False,
/// with nothing started, when the world cannot be edited or the entity is
/// not alive.
bool begin_entity_rename(runtime::Entity entity) noexcept;

/// True while `entity` is the one being renamed.
bool entity_rename_active_for(runtime::Entity entity) noexcept;

/// The rename in progress, whose buffer the row's text field edits.
EntityRenameState &entity_rename_state() noexcept;

/// Ends the rename, giving the entity the typed name as one undoable edit
/// (adding a name when it had none). An empty or unchanged name, or an
/// entity no longer alive, changes nothing. True when the name changed.
bool commit_entity_rename() noexcept;

/// Ends the rename without changing the name.
void cancel_entity_rename() noexcept;

} // namespace engine::editor
