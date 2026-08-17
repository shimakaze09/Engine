// Declares multi-entity Inspector support (issue #159): which persistent
// components the current selection shares, per-field mixed-value detection,
// and the single undoable command that applies one field's new value to
// every selected entity at once without disturbing any other (possibly
// still-mixed) field.

#pragma once

#include <array>
#include <cstddef>

#include "editor_component_registry.h"

namespace engine::editor {

/// Returns a pointer to the ComponentEditSnapshot member for `type`
/// (&snapshot->rigidBody, &snapshot->collider, ...), generated from the
/// same persistent-component registry as capture/apply_component_snapshot
/// so a new registry row cannot silently fall through the multi-edit path
/// without a compile error; nullptr is never returned for a valid
/// ComponentEditType.
void *component_member_ptr(ComponentEditType type,
                           ComponentEditSnapshot *snapshot) noexcept;
const void *component_member_ptr(ComponentEditType type,
                                 const ComponentEditSnapshot &snapshot) noexcept;

/// Fills `outCommon[i]` true when every currently-selected entity carries a
/// component of ComponentEditType(i); all false when the world is unbound
/// or nothing is selected. Recomputed fresh each call -- the Inspector's
/// own bounded selection (EditorSession::kMaxSelectedEntities), never a
/// hot path.
void compute_selection_common_components(
    std::array<bool, kComponentEditTypeCount> *outCommon) noexcept;

/// True when a reflected field at byte range [fieldOffset, fieldOffset +
/// fieldSize) of `type`'s component holds a different value on at least
/// one selected entity versus the first selected entity -- the Inspector's
/// mixed-value indicator. False (including "selection too small to be
/// mixed") when fewer than two entities are selected or any entity lacks
/// the component.
bool selection_field_is_mixed(ComponentEditType type, std::size_t fieldOffset,
                              std::size_t fieldSize) noexcept;

/// Copies the first selected entity's component of `type` into
/// `outRepresentative` (the seed value the multi Inspector's per-field
/// widgets edit); false when the selection is empty, the world is
/// unbound, or the first entity lacks the component.
bool selection_representative_component(
    ComponentEditType type, ComponentEditSnapshot *outRepresentative) noexcept;

/// Applies the field at [fieldOffset, fieldOffset + fieldSize) of
/// `fieldSource`'s `type` member to every currently-selected entity's own
/// component of `type`, leaving every other field at each entity's
/// existing value (so editing one mixed field never clobbers another
/// still-mixed field), as ONE undoable command (one gesture, one undo
/// step, per the command-history contract). Atomic: if any selected
/// entity rejects the write, every entity already touched this call is
/// rolled back to its prior value before returning and no history entry
/// is pushed. False when the selection is empty, any selected entity
/// currently lacks the component, or the write was rolled back.
bool apply_multi_field_edit(ComponentEditType type, std::size_t fieldOffset,
                            std::size_t fieldSize,
                            const ComponentEditSnapshot &fieldSource) noexcept;

/// Removes `type`'s component from every currently-selected entity, as ONE
/// undoable command with the same atomic all-or-nothing rollback as
/// apply_multi_field_edit. False when the selection is empty or any
/// selected entity currently lacks the component (removal is offered only
/// for components compute_selection_common_components already reported
/// present on everyone).
bool apply_multi_component_remove(ComponentEditType type) noexcept;

/// True when `type` has a reflected per-field section in the multi-object
/// Inspector inventory.
bool multi_edit_section_listed(ComponentEditType type) noexcept;

/// True when `type` is explicitly deferred from per-field multi-edit (the
/// custom-drawer components tracked by issue #223). Every registry edit
/// type is exactly one of listed or deferred, enforced at compile time.
bool multi_edit_type_deferred(ComponentEditType type) noexcept;

/// Draws the Inspector's multi-selection body (issue #159): one section per
/// component common to every selected entity, each reflected field shown
/// with a "(mixed)" suffix when the selection disagrees on its value, plus
/// the shared Delete-selected and identity summary. No-ops when the
/// selection has fewer than two entities (the single-entity Inspector
/// handles that case). Draw-only; not exercised by headless tests (per
/// CLAUDE.md's draw-code exemption) -- the functions above it carry the
/// tested contract.
void draw_multi_select_inspector_panel() noexcept;

} // namespace engine::editor
