// Verifies editor command/history transaction behavior (issue #117): a
// failed command execute is not recorded and preserves the redo stack, a
// failed undo/redo leaves the history cursor in place, and the entity
// delete command's subtree restore is all-or-nothing under entity
// capacity pressure while preserving persistent ids.

#include "editor_commands.h"
#include "engine/editor/command_history.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace {

using engine::editor::CommandHistory;
using engine::editor::ComponentEditCommand;
using engine::editor::ComponentEditSnapshot;
using engine::editor::ComponentEditType;
using engine::editor::EntityCreateCommand;
using engine::runtime::Entity;
using engine::runtime::MeshComponent;
using engine::runtime::NameComponent;
using engine::runtime::PersistentId;
using engine::runtime::Transform;
using engine::runtime::World;

/// Creates bare entities until the world is full; returns the fillers.
std::vector<Entity> fill_entity_capacity(World &world) {
  std::vector<Entity> fillers{};
  for (;;) {
    const Entity entity = world.create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      break;
    }
    fillers.push_back(entity);
  }
  return fillers;
}

/// EXPECTATION: a command whose execute fails (here: an entity create
/// whose collider insertion is rejected) is not entered into history, the
/// world keeps no partial entity, and a previously available redo stack
/// survives the failed execute.
int check_failed_execute_not_recorded() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 10;
  }
  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  const Entity entity = world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return finish(11);
  }

  // One applied-then-undone name edit arms the redo stack.
  auto *nameCommand = new (std::nothrow) ComponentEditCommand();
  if (nameCommand == nullptr) {
    return finish(12);
  }
  nameCommand->entity = entity;
  nameCommand->persistentId = world->persistent_id(entity);
  nameCommand->type = ComponentEditType::Name;
  nameCommand->beforeExists = false;
  nameCommand->afterExists = true;
  std::snprintf(nameCommand->after.name.name,
                sizeof(nameCommand->after.name.name), "%s", "Named");

  CommandHistory history{};
  history.execute(nameCommand);
  history.undo();
  if (history.can_undo() || !history.can_redo()) {
    return finish(13);
  }

  // Zero half-extents fail collider ingress validation, so this create
  // cannot fully apply.
  const std::size_t aliveBefore = world->alive_entity_count();
  auto *badCreate = new (std::nothrow) EntityCreateCommand();
  if (badCreate == nullptr) {
    return finish(14);
  }
  badCreate->hasCollider = true;
  badCreate->colliderComponent.halfExtents =
      engine::math::Vec3(0.0F, 0.0F, 0.0F);
  history.execute(badCreate);

  if (world->alive_entity_count() != aliveBefore) {
    return finish(15);
  }
  if (history.can_undo()) {
    return finish(16);
  }
  if (!history.can_redo()) {
    return finish(17);
  }

  // The surviving redo entry still re-applies.
  history.redo();
  NameComponent restored{};
  if (!world->get_name_component(entity, &restored) ||
      (std::strcmp(restored.name, "Named") != 0)) {
    return finish(18);
  }
  return finish(0);
}

/// EXPECTATION: undoing a subtree delete under entity-capacity pressure
/// is transactional — a failed undo keeps the delete on the cursor, opens
/// no redo, restores no partial member set, and a later retry with enough
/// capacity restores every member under its original persistent id; the
/// following redo/undo cycle keeps working.
int check_subtree_restore_is_transactional() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 30;
  }
  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  session.commandHistory.clear();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.commandHistory.clear();
    session.world = previousWorld;
    return result;
  };

  // Parent (name + mesh) → child (name) → grandchild.
  const Entity parent = world->create_scene_object();
  if (parent == engine::runtime::kInvalidEntity) {
    return finish(31);
  }
  NameComponent parentName{};
  std::snprintf(parentName.name, sizeof(parentName.name), "%s", "Parent");
  MeshComponent parentMesh{};
  parentMesh.meshAssetId = 42ULL;
  if (!world->add_name_component(parent, parentName) ||
      !world->add_mesh_component(parent, parentMesh)) {
    return finish(32);
  }
  const PersistentId parentId = world->persistent_id(parent);

  Transform childTransform{};
  childTransform.parentId = parentId;
  const Entity child = world->create_scene_object(childTransform);
  if (child == engine::runtime::kInvalidEntity) {
    return finish(33);
  }
  NameComponent childName{};
  std::snprintf(childName.name, sizeof(childName.name), "%s", "Child");
  if (!world->add_name_component(child, childName)) {
    return finish(34);
  }
  const PersistentId childId = world->persistent_id(child);

  Transform grandTransform{};
  grandTransform.parentId = childId;
  const Entity grand = world->create_scene_object(grandTransform);
  if (grand == engine::runtime::kInvalidEntity) {
    return finish(35);
  }
  const PersistentId grandId = world->persistent_id(grand);

  // Delete the subtree through the production editor entry point.
  if (!engine::editor::execute_entity_delete(parent) ||
      (world->alive_entity_count() != 0U)) {
    return finish(36);
  }

  // Consume every freed entity slot, then attempt the undo: with zero
  // capacity nothing can be restored.
  std::vector<Entity> fillers = fill_entity_capacity(*world);
  const std::size_t fullCount = world->alive_entity_count();
  session.commandHistory.undo();
  if (!session.commandHistory.can_undo()) {
    return finish(37);
  }
  if (session.commandHistory.can_redo()) {
    return finish(38);
  }
  if ((world->find_entity_by_persistent_id(parentId) !=
       engine::runtime::kInvalidEntity) ||
      (world->find_entity_by_persistent_id(childId) !=
       engine::runtime::kInvalidEntity) ||
      (world->find_entity_by_persistent_id(grandId) !=
       engine::runtime::kInvalidEntity)) {
    return finish(39);
  }
  if (world->alive_entity_count() != fullCount) {
    return finish(40);
  }

  // Redo after the failed undo is a no-op: the delete never left the
  // cursor, so there is nothing to re-apply.
  session.commandHistory.redo();
  if (world->alive_entity_count() != fullCount) {
    return finish(41);
  }

  // Two free slots let parent and child re-create but not the grandchild:
  // the undo must roll both back out again instead of leaving a partial
  // subtree.
  if (!world->destroy_entity(fillers.back())) {
    return finish(42);
  }
  fillers.pop_back();
  if (!world->destroy_entity(fillers.back())) {
    return finish(43);
  }
  fillers.pop_back();
  session.commandHistory.undo();
  if (!session.commandHistory.can_undo()) {
    return finish(44);
  }
  if ((world->find_entity_by_persistent_id(parentId) !=
       engine::runtime::kInvalidEntity) ||
      (world->find_entity_by_persistent_id(childId) !=
       engine::runtime::kInvalidEntity) ||
      (world->find_entity_by_persistent_id(grandId) !=
       engine::runtime::kInvalidEntity)) {
    return finish(45);
  }
  if (world->alive_entity_count() != (fullCount - 2U)) {
    return finish(46);
  }

  // With capacity for all three members the undo completes atomically,
  // preserving persistent ids, components, and parent links.
  if (!world->destroy_entity(fillers.back())) {
    return finish(47);
  }
  fillers.pop_back();
  session.commandHistory.undo();
  if (session.commandHistory.can_undo() ||
      !session.commandHistory.can_redo()) {
    return finish(48);
  }
  const Entity restoredParent = world->find_entity_by_persistent_id(parentId);
  const Entity restoredChild = world->find_entity_by_persistent_id(childId);
  const Entity restoredGrand = world->find_entity_by_persistent_id(grandId);
  if ((restoredParent == engine::runtime::kInvalidEntity) ||
      (restoredChild == engine::runtime::kInvalidEntity) ||
      (restoredGrand == engine::runtime::kInvalidEntity)) {
    return finish(49);
  }
  NameComponent restoredName{};
  MeshComponent restoredMesh{};
  Transform restoredChildTransform{};
  if (!world->get_name_component(restoredParent, &restoredName) ||
      (std::strcmp(restoredName.name, "Parent") != 0) ||
      !world->get_mesh_component(restoredParent, &restoredMesh) ||
      (restoredMesh.meshAssetId != 42ULL) ||
      !world->get_transform(restoredChild, &restoredChildTransform) ||
      (restoredChildTransform.parentId != parentId)) {
    return finish(50);
  }

  // The redo/undo cycle keeps operating on the restored subtree.
  session.commandHistory.redo();
  if ((world->find_entity_by_persistent_id(parentId) !=
       engine::runtime::kInvalidEntity) ||
      session.commandHistory.can_undo() == false) {
    return finish(51);
  }
  session.commandHistory.undo();
  if ((world->find_entity_by_persistent_id(parentId) ==
       engine::runtime::kInvalidEntity) ||
      (world->find_entity_by_persistent_id(grandId) ==
       engine::runtime::kInvalidEntity)) {
    return finish(52);
  }
  return finish(0);
}

/// EXPECTATION: a failed redo (entity re-create under a full world) does
/// not advance the cursor, so the redo stays available and succeeds once
/// capacity returns.
int check_failed_redo_holds_cursor() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 70;
  }
  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  session.commandHistory.clear();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.commandHistory.clear();
    session.world = previousWorld;
    return result;
  };

  const Entity created = engine::editor::execute_entity_create();
  if (created == engine::runtime::kInvalidEntity) {
    return finish(71);
  }
  const PersistentId createdId = world->persistent_id(created);
  session.commandHistory.undo();
  if (world->find_entity_by_persistent_id(createdId) !=
      engine::runtime::kInvalidEntity) {
    return finish(72);
  }

  std::vector<Entity> fillers = fill_entity_capacity(*world);
  const std::size_t fullCount = world->alive_entity_count();
  session.commandHistory.redo();
  if (!session.commandHistory.can_redo()) {
    return finish(73);
  }
  if (world->alive_entity_count() != fullCount) {
    return finish(74);
  }

  if (!world->destroy_entity(fillers.back())) {
    return finish(75);
  }
  fillers.pop_back();
  session.commandHistory.redo();
  if (world->find_entity_by_persistent_id(createdId) ==
      engine::runtime::kInvalidEntity) {
    return finish(76);
  }
  if (session.commandHistory.can_redo()) {
    return finish(77);
  }
  return finish(0);
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_failed_execute_not_recorded();
  if (result != 0) {
    std::fprintf(stderr, "editor_command_transaction_test failed: %d\n",
                 result);
    return result;
  }
  result = check_subtree_restore_is_transactional();
  if (result != 0) {
    std::fprintf(stderr, "editor_command_transaction_test failed: %d\n",
                 result);
    return result;
  }
  result = check_failed_redo_holds_cursor();
  if (result != 0) {
    std::fprintf(stderr, "editor_command_transaction_test failed: %d\n",
                 result);
    return result;
  }
  return 0;
}
