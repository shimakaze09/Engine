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

} // namespace engine::editor
