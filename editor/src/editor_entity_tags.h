// Declares the Inspector's Tags row, as Unity's Tag field sits under the
// object's name: the selected entity's gameplay tags as buttons that
// remove them and a field that adds the one typed in, each change one
// undoable edit, plus the pure step that decides what a typed tag does.

#pragma once

#include "engine/runtime/world.h"

namespace engine::editor {

/// What typing `typed` into the Tags field does to `current`: the edited
/// set in `*out` and nullptr when it adds the tag, or the reason shown
/// under the field when it does not (`*out` is then `current`). A tag the
/// set already holds, ignoring case, is no change and no error.
const char *apply_typed_tag(const runtime::TagSetComponent &current,
                            const char *typed,
                            runtime::TagSetComponent *out) noexcept;

/// Records `after` as the entity's tags in one undoable command: an add
/// when `before` is null (the entity had no tags), a remove when `after`
/// holds none, an edit otherwise.
void commit_entity_tags(runtime::Entity entity,
                        const runtime::TagSetComponent *before,
                        const runtime::TagSetComponent &after) noexcept;

/// Draws the Tags row for `entity`. Adding the first tag adds the Tags
/// component and removing the last one removes it, so an untagged entity
/// carries nothing; every change goes through the command history.
/// `editable` false draws the tags without the controls.
void draw_entity_tags_row(runtime::Entity entity, bool editable) noexcept;

} // namespace engine::editor
