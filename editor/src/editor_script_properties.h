// Declares the Script section's property fields: the properties each of
// the entity's behaviours declares, read by a static scan of its script's
// text, drawn as typed fields that edit the entity's ScriptProperties
// overrides through the command history.

#pragma once

#include <cstddef>

#include "engine/runtime/world.h"
#include "engine/scripting/script_property_scan.h"

namespace engine::editor {

/// The scan of the script at `scriptPath` (a VFS path), cached and scanned
/// again when the file changes; null when the file cannot be read.
const scripting::ScriptPropertySchema *
script_property_schema(const char *scriptPath) noexcept;

/// The label a property name is shown with: words split at '_' and at a
/// lower-to-upper case change, each capitalized ("move_speed" and
/// "moveSpeed" both read "Move Speed"), as Unity labels a field.
void script_property_label(const char *name, char *out,
                           std::size_t capacity) noexcept;

/// The override set `current` becomes when behaviour `behaviour`'s
/// property `name`, whose script default is `defaultValue`, is set to
/// `value`: a value equal to the default clears the override, so only
/// differences are stored. False, with `out` unchanged, when the set
/// cannot take another override.
bool script_properties_with_value(
    const runtime::ScriptPropertiesComponent &current, std::size_t behaviour,
    const char *name,
    const runtime::ScriptPropertyValue &value,
    const runtime::ScriptPropertyValue &defaultValue,
    runtime::ScriptPropertiesComponent *out) noexcept;

/// Draws the fields of the properties the script at `scriptPath`, the
/// entity's behaviour `behaviour`, declares, under that behaviour's path
/// field. A change is staged as an undoable edit of the entity's
/// ScriptProperties overrides; a value that differs from the script's
/// default is highlighted and offers Reset to Default. Values the script no
/// longer declares are listed and kept until removed, and what the scan
/// could not read is shown with its line.
void draw_script_property_fields(runtime::Entity entity, std::size_t behaviour,
                                 const char *scriptPath) noexcept;

/// Forgets the cached scans, so the next draw reads the scripts again.
void reset_script_property_schemas() noexcept;

} // namespace engine::editor
