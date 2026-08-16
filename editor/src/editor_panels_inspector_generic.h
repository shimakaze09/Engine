// Declares the Inspector's generic, metadata-driven reflected-field drawer
// (issue #156): walks core::TypeRegistry for a component type and renders
// each field using editor_inspector_metadata's semantic hints instead of a
// per-Kind raw-value widget.

#pragma once

namespace engine::editor {

/// Draws every reflected field of `typeName` found at `instance`, applying
/// editor_inspector_metadata display/range/widget hints; returns true if
/// any field's value changed this frame. Fields marked advanced in their
/// metadata are skipped unless `showAdvanced` is set (the Inspector's
/// progressive-disclosure Advanced toggle). A field with no widget in
/// core::TypeField::Kind (there is no Uint64/Enum kind yet) simply cannot
/// be reached this way -- those fields belong to a component's custom
/// drawer instead (editor_panels_inspector_custom.h).
bool draw_reflected_component_fields(const char *typeName, void *instance,
                                     bool showAdvanced) noexcept;

/// Draws exactly one reflected field of `typeName` at `instance` by field
/// name, using the same per-Kind widgets and metadata hints as
/// draw_reflected_component_fields; the multi-select Inspector (issue #159)
/// calls this once per field on a scratch representative instance instead
/// of the whole-component loop, so it can tell which single field changed
/// and never clobbers a sibling field that is still mixed across the
/// selection. `labelSuffix` (nullptr for none) is appended verbatim to the
/// field's label -- the multi Inspector passes " (mixed)". Returns true if
/// the field's value changed this frame; false (no-op) on an unknown type/
/// field name or an advanced field hidden by `showAdvanced`.
bool draw_reflected_field(const char *typeName, const char *fieldName,
                         void *instance, const char *labelSuffix,
                         bool showAdvanced) noexcept;

} // namespace engine::editor
