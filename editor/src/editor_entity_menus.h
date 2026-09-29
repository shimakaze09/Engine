// Declares the right-click menus of the windows that edit entities, the
// Entities panel and the Scene view, as Unity's Hierarchy and Scene view
// menus are: one on an entity (its edits, Rename, Frame Selected, and
// creating a child) and one on empty space (Create Empty, 3D Object,
// Paste). Every item is an action table row or a primitive, so its label,
// chord and enabled state agree with the Edit and Entity menus. Drawing
// only reports the choice; the caller runs it once it is done iterating
// what it drew, so no menu edits the world mid-walk.

#pragma once

#include <cstdint>

#include "editor_commands.h"
#include "editor_shortcuts.h"

namespace engine::editor {

/// What a menu item asked for.
struct EntityMenuChoice final {
  enum class Kind : std::uint8_t {
    None,
    /// Run `action` from the action table.
    Action,
    /// Create an empty entity at the menu's placement.
    CreateEmpty,
    /// Spawn `primitive` at the menu's placement.
    CreatePrimitive,
  };
  Kind kind = Kind::None;
  EditorAction action = EditorAction::Count;
  EditorPrimitive primitive = EditorPrimitive::Cube;
};

/// Draws the items of the menu on an entity, which the caller has made
/// (part of) the selection: Copy, Paste, Paste As Child, Duplicate,
/// Delete, Rename, Frame Selected, Create Empty Child and 3D Object Child.
/// A 3D Object Child is created under the entity, so the caller runs it
/// with the entity as the placement's parent.
EntityMenuChoice draw_entity_menu_items() noexcept;

/// Draws the items of the menu on empty space: Create Empty, 3D Object,
/// and Paste.
EntityMenuChoice draw_empty_space_menu_items() noexcept;

/// Draws one item per built-in primitive (Cube, Sphere, Cylinder,
/// Capsule, Pyramid, Plane), each disabled while the world cannot be
/// edited: the one list every 3D Object menu shows.
EntityMenuChoice draw_primitive_menu_items() noexcept;

/// Runs `choice`: an action through the action table, a creation at
/// `placement`. A created entity becomes the selection. False when nothing
/// ran or it was refused.
bool run_entity_menu_choice(const EntityMenuChoice &choice,
                            const EntitySpawnPlacement &placement) noexcept;

} // namespace engine::editor
