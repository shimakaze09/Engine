// Implements the right-click menus of the Entities panel and the Scene
// view: the items on an entity and on empty space, and running what was
// chosen.

#include "editor_entity_menus.h"

#include "imgui.h"

#include "editor_session.h"

namespace engine::editor {

namespace {

/// The primitives a 3D Object submenu offers, in the Entity menu's order.
constexpr struct {
  const char *label;
  EditorPrimitive primitive;
} kPrimitives[] = {
    {"Cube", EditorPrimitive::Cube},
    {"Sphere", EditorPrimitive::Sphere},
    {"Cylinder", EditorPrimitive::Cylinder},
    {"Capsule", EditorPrimitive::Capsule},
    {"Pyramid", EditorPrimitive::Pyramid},
    {"Plane", EditorPrimitive::Plane},
};

/// Draws an action's item; `choice` records it when clicked.
void action_item(EditorAction action, EntityMenuChoice *choice) noexcept {
  if (editor_action_menu_item_clicked(action)) {
    choice->kind = EntityMenuChoice::Kind::Action;
    choice->action = action;
  }
}

/// Draws a submenu of primitives; `choice` records the one clicked.
void primitive_submenu(const char *label, EntityMenuChoice *choice) noexcept {
  if (!ImGui::BeginMenu(label, world_is_editable())) {
    return;
  }
  const EntityMenuChoice picked = draw_primitive_menu_items();
  if (picked.kind != EntityMenuChoice::Kind::None) {
    *choice = picked;
  }
  ImGui::EndMenu();
}

} // namespace

EntityMenuChoice draw_primitive_menu_items() noexcept {
  EntityMenuChoice choice{};
  const bool editable = world_is_editable();
  for (const auto &item : kPrimitives) {
    if (ImGui::MenuItem(item.label, nullptr, false, editable)) {
      choice.kind = EntityMenuChoice::Kind::CreatePrimitive;
      choice.primitive = item.primitive;
    }
  }
  return choice;
}

EntityMenuChoice draw_entity_menu_items() noexcept {
  EntityMenuChoice choice{};
  for (const EditorAction action :
       {EditorAction::Copy, EditorAction::Paste, EditorAction::PasteAsChild,
        EditorAction::Duplicate, EditorAction::Delete}) {
    action_item(action, &choice);
  }
  ImGui::Separator();
  action_item(EditorAction::Rename, &choice);
  action_item(EditorAction::FrameSelected, &choice);
  ImGui::Separator();
  action_item(EditorAction::CreateEmptyChild, &choice);
  primitive_submenu("3D Object Child", &choice);
  return choice;
}

EntityMenuChoice draw_empty_space_menu_items() noexcept {
  EntityMenuChoice choice{};
  // Create Empty is placed by the menu (at the cursor in the Scene view),
  // so it is a creation here, drawn as the action's row.
  if (ImGui::MenuItem(editor_action_label(EditorAction::CreateEmpty),
                      editor_shortcut_text(EditorAction::CreateEmpty), false,
                      editor_action_enabled(EditorAction::CreateEmpty))) {
    choice.kind = EntityMenuChoice::Kind::CreateEmpty;
  }
  primitive_submenu("3D Object", &choice);
  ImGui::Separator();
  action_item(EditorAction::Paste, &choice);
  return choice;
}

bool run_entity_menu_choice(const EntityMenuChoice &choice,
                            const EntitySpawnPlacement &placement) noexcept {
  runtime::Entity created = runtime::kInvalidEntity;
  switch (choice.kind) {
  case EntityMenuChoice::Kind::Action:
    return run_editor_action(choice.action);
  case EntityMenuChoice::Kind::CreateEmpty:
    created = execute_entity_create(placement);
    break;
  case EntityMenuChoice::Kind::CreatePrimitive:
    created = execute_primitive_spawn(choice.primitive, placement);
    break;
  case EntityMenuChoice::Kind::None:
  default:
    return false;
  }
  if (created == runtime::kInvalidEntity) {
    return false;
  }
  select_entity(created, false);
  return true;
}

} // namespace engine::editor
