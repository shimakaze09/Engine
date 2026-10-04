// Declares the Script section's behaviour list: each behaviour the entity
// runs with its enabled toggle, script path and property fields, controls
// to add, remove and reorder behaviours, and the undoable command that
// changes the list and the property overrides together, so a removal or
// reorder is one undo step and every override stays with its script.

#pragma once

#include <cstddef>

#include "editor_commands.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// Undoable change of an entity's behaviour list together with its
/// property overrides, stored as before/after snapshots of both.
struct ScriptBehavioursCommand final : EditorCommand {
  runtime::Entity entity{};
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  bool scriptBeforeExists = false;
  bool propertiesBeforeExists = false;
  bool propertiesAfterExists = false;
  ComponentEditSnapshot before{};
  ComponentEditSnapshot after{};

  bool execute() noexcept override {
    return apply_state(true, propertiesAfterExists, after);
  }
  bool undo() noexcept override {
    return apply_state(scriptBeforeExists, propertiesBeforeExists, before);
  }
  std::size_t memory_bytes() const noexcept override { return sizeof(*this); }

private:
  /// Applies one endpoint: the list first, then the overrides.
  bool apply_state(bool scriptExists, bool propertiesExist,
                   const ComponentEditSnapshot &snapshot) noexcept;
};

/// Replaces the entity's behaviour list with `script` and its overrides
/// with `properties` as one undoable command. False, with nothing changed,
/// when the World refuses either or the command cannot be recorded.
bool execute_script_behaviours_edit(
    runtime::Entity entity, const runtime::ScriptComponent &script,
    const runtime::ScriptPropertiesComponent &properties) noexcept;

/// Draws the entity's behaviours in `script`: per behaviour an enabled
/// checkbox, its script path, move and remove buttons and its property
/// fields, then a picker that adds a behaviour. An enabled toggle or a
/// path change edits `script` and returns true, staged by the section like
/// any field; add, remove and reorder run as one ScriptBehavioursCommand
/// each and return false. Overrides naming a behaviour the list no longer
/// has are listed with Remove.
bool draw_script_behaviour_list(runtime::Entity entity,
                                runtime::ScriptComponent &script) noexcept;

} // namespace engine::editor
