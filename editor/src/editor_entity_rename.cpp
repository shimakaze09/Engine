// Implements renaming an entity in place: the rename in progress, and its
// commit through the Inspector's own undoable name edit, so a rename and a
// typed Name field undo alike.

#include "editor_entity_rename.h"

#include <cstring>

#include "editor_commands.h"
#include "editor_session.h"

namespace engine::editor {

namespace {

EntityRenameState g_rename{};

/// The entity being renamed, when it is still alive.
runtime::Entity rename_target() noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) ||
      (g_rename.target == runtime::kInvalidPersistentId)) {
    return runtime::kInvalidEntity;
  }
  return world->find_entity_by_persistent_id(g_rename.target);
}

} // namespace

bool begin_entity_rename(runtime::Entity entity) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || !world_is_editable() || !world->is_alive(entity)) {
    return false;
  }
  g_rename = EntityRenameState{};
  runtime::NameComponent name{};
  if (world->get_name_component(entity, &name)) {
    std::memcpy(g_rename.buffer, name.name, sizeof(g_rename.buffer));
  }
  g_rename.target = world->persistent_id(entity);
  g_rename.focusPending = true;
  return true;
}

bool entity_rename_active_for(runtime::Entity entity) noexcept {
  return (entity != runtime::kInvalidEntity) && (rename_target() == entity);
}

EntityRenameState &entity_rename_state() noexcept { return g_rename; }

bool commit_entity_rename() noexcept {
  const runtime::Entity entity = rename_target();
  runtime::NameComponent typed{};
  std::memcpy(typed.name, g_rename.buffer, sizeof(typed.name));
  typed.name[sizeof(typed.name) - 1U] = '\0';
  g_rename = EntityRenameState{};
  runtime::World *const world = editor_session().world;
  if ((entity == runtime::kInvalidEntity) || (typed.name[0] == '\0') ||
      !world_is_editable()) {
    return false;
  }
  runtime::NameComponent current{};
  const bool hasName = world->get_name_component(entity, &current);
  if (hasName && (std::strcmp(current.name, typed.name) == 0)) {
    return false;
  }
  ComponentEditSnapshot after{};
  after.name = typed;
  if (!hasName) {
    execute_component_add(entity, ComponentEditType::Name, after);
    runtime::NameComponent added{};
    return world->get_name_component(entity, &added) &&
           (std::strcmp(added.name, typed.name) == 0);
  }
  ComponentEditSnapshot before{};
  before.name = current;
  if (!inspector_stage_component_edit(entity, ComponentEditType::Name, before,
                                      after)) {
    return false;
  }
  inspector_commit_pending_edit();
  return true;
}

void cancel_entity_rename() noexcept { g_rename = EntityRenameState{}; }

} // namespace engine::editor
