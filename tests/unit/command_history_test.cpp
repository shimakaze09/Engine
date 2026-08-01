// Verifies editor command history ownership behavior.

#include "editor_commands.h"
#include "editor_transform_util.h"
#include "engine/editor/command_history.h"
#include "engine/math/transform.h"
#include "engine/runtime/world.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

namespace {

int g_liveCommands = 0;
int g_destroyedCommands = 0;
int g_executeCount = 0;
int g_undoCount = 0;
int g_redoCount = 0;

void reset_counts() noexcept {
  g_liveCommands = 0;
  g_destroyedCommands = 0;
  g_executeCount = 0;
  g_undoCount = 0;
  g_redoCount = 0;
}

struct CountingCommand final : engine::editor::EditorCommand {
  CountingCommand() noexcept { ++g_liveCommands; }

  ~CountingCommand() override {
    --g_liveCommands;
    ++g_destroyedCommands;
  }

  void execute() noexcept override { ++g_executeCount; }

  void undo() noexcept override { ++g_undoCount; }

  void redo() noexcept override { ++g_redoCount; }
};

engine::editor::EditorCommand *make_command() noexcept {
  return new (std::nothrow) CountingCommand();
}

/// Returns whether two editor-transform scalar values match tightly.
bool nearly_equal(float lhs, float rhs) noexcept {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

/// Returns whether two transform matrices represent the same affine mapping.
bool matrices_nearly_equal(const engine::math::Mat4 &lhs,
                           const engine::math::Mat4 &rhs) noexcept {
  for (std::size_t column = 0U; column < 4U; ++column) {
    if (!nearly_equal(lhs.columns[column].x, rhs.columns[column].x) ||
        !nearly_equal(lhs.columns[column].y, rhs.columns[column].y) ||
        !nearly_equal(lhs.columns[column].z, rhs.columns[column].z) ||
        !nearly_equal(lhs.columns[column].w, rhs.columns[column].w)) {
      return false;
    }
  }
  return true;
}

/// Verifies a parented gizmo world matrix converts back to local TRS and
/// preserves the selected object's parent identity.
int check_parented_gizmo_world_to_local_conversion() noexcept {
  constexpr float kHalfPi = 1.57079632679F;
  const engine::math::Mat4 parentWorld = engine::math::compose_trs(
      engine::math::Vec3(10.0F, -2.0F, 5.0F),
      engine::math::from_axis_angle(engine::math::Vec3(0.0F, 0.0F, 1.0F),
                                    kHalfPi),
      engine::math::Vec3(2.0F, 2.0F, 2.0F));

  engine::runtime::Transform expected{};
  expected.position = engine::math::Vec3(4.0F, -1.0F, 2.0F);
  expected.rotation = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 1.0F, 0.0F), kHalfPi);
  expected.scale = engine::math::Vec3(0.5F, 1.5F, 2.0F);
  expected.parentId = 77U;

  const engine::math::Mat4 expectedLocalMatrix = engine::math::compose_trs(
      expected.position, expected.rotation, expected.scale);
  const engine::math::Mat4 manipulatedWorld =
      engine::math::mul(parentWorld, expectedLocalMatrix);

  engine::runtime::Transform converted{};
  if (!engine::editor::world_matrix_to_local_transform(
          manipulatedWorld, &parentWorld, expected, &converted)) {
    return 1;
  }
  if (converted.parentId != expected.parentId) {
    return 2;
  }

  const engine::math::Mat4 convertedLocalMatrix = engine::math::compose_trs(
      converted.position, converted.rotation, converted.scale);
  return matrices_nearly_equal(convertedLocalMatrix, expectedLocalMatrix) ? 0
                                                                          : 3;
}

int check_destructor_releases_commands() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    history.execute(make_command());
    history.execute(make_command());
    if (g_liveCommands != 2) {
      return 10;
    }
  }

  if ((g_liveCommands != 0) || (g_destroyedCommands != 2)) {
    return 11;
  }
  return 0;
}

int check_redo_entries_are_released() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    history.execute(make_command());
    history.execute(make_command());
    history.undo();
    if (!history.can_redo()) {
      return 20;
    }

    history.execute(make_command());
    if (history.can_redo()) {
      return 21;
    }
    if ((g_liveCommands != 2) || (g_destroyedCommands != 1)) {
      return 22;
    }
  }

  if ((g_liveCommands != 0) || (g_destroyedCommands != 3)) {
    return 23;
  }
  return 0;
}

int check_capacity_eviction_releases_oldest_command() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    for (std::size_t i = 0U;
         i < engine::editor::CommandHistory::kMaxHistory + 1U; ++i) {
      history.execute(make_command());
    }

    if ((g_liveCommands !=
         static_cast<int>(engine::editor::CommandHistory::kMaxHistory)) ||
        (g_destroyedCommands != 1)) {
      return 30;
    }
  }

  if ((g_liveCommands != 0) ||
      (g_destroyedCommands !=
       static_cast<int>(engine::editor::CommandHistory::kMaxHistory + 1U))) {
    return 31;
  }
  return 0;
}

int check_undo_redo_dispatch() noexcept {
  reset_counts();
  engine::editor::CommandHistory history{};
  history.execute(make_command());

  if ((g_executeCount != 1) || !history.can_undo() || history.can_redo()) {
    return 40;
  }

  history.undo();
  if ((g_undoCount != 1) || history.can_undo() || !history.can_redo()) {
    return 41;
  }

  history.redo();
  if ((g_redoCount != 1) || !history.can_undo() || history.can_redo()) {
    return 42;
  }
  return 0;
}

/// Undo and redo must not target a replacement entity that reuses the slot.
int check_component_command_rejects_recycled_entity() noexcept {
  using engine::editor::ComponentEditCommand;
  using engine::editor::ComponentEditSnapshot;
  using engine::editor::ComponentEditType;
  using engine::runtime::Entity;
  using engine::runtime::NameComponent;
  using engine::runtime::World;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 50;
  }

  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  const Entity original = world->create_entity();
  if (original == engine::runtime::kInvalidEntity) {
    return finish(51);
  }

  ComponentEditSnapshot added{};
  std::snprintf(added.name.name, sizeof(added.name.name), "%s", "Original");
  auto *command = new (std::nothrow) ComponentEditCommand();
  if (command == nullptr) {
    return finish(52);
  }
  command->entity = original;
  command->type = ComponentEditType::Name;
  command->beforeExists = false;
  command->afterExists = true;
  command->after = added;

  engine::editor::CommandHistory history{};
  history.execute(command);

  NameComponent current{};
  if (!world->get_name_component(original, &current) ||
      (std::strcmp(current.name, "Original") != 0)) {
    return finish(53);
  }
  if (!world->destroy_entity(original)) {
    return finish(54);
  }

  const Entity replacement = world->create_entity();
  if ((replacement == engine::runtime::kInvalidEntity) ||
      (replacement.index != original.index) ||
      (replacement.generation == original.generation)) {
    return finish(55);
  }

  NameComponent replacementName{};
  std::snprintf(replacementName.name, sizeof(replacementName.name), "%s",
                "Replacement");
  if (!world->add_name_component(replacement, replacementName)) {
    return finish(56);
  }

  history.undo();
  NameComponent afterUndo{};
  if (!world->get_name_component(replacement, &afterUndo) ||
      (std::strcmp(afterUndo.name, "Replacement") != 0)) {
    return finish(57);
  }

  history.redo();
  NameComponent afterRedo{};
  if (!world->get_name_component(replacement, &afterRedo) ||
      (std::strcmp(afterRedo.name, "Replacement") != 0)) {
    return finish(58);
  }

  return finish(0);
}

/// Commands that captured a persistent id must re-target the entity after
/// it is destroyed and re-created under the same persistent id.
int check_commands_resolve_persistent_id_after_recreate() noexcept {
  using engine::editor::ComponentEditCommand;
  using engine::editor::ComponentEditSnapshot;
  using engine::editor::ComponentEditType;
  using engine::editor::TransformEditCommand;
  using engine::runtime::Entity;
  using engine::runtime::NameComponent;
  using engine::runtime::Transform;
  using engine::runtime::World;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 60;
  }

  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  const Entity first = world->create_scene_object();
  if (first == engine::runtime::kInvalidEntity) {
    return finish(61);
  }
  const engine::runtime::PersistentId persistentId =
      world->persistent_id(first);
  if (persistentId == engine::runtime::kInvalidPersistentId) {
    return finish(62);
  }

  ComponentEditSnapshot added{};
  std::snprintf(added.name.name, sizeof(added.name.name), "%s", "Tracked");
  auto *nameCommand = new (std::nothrow) ComponentEditCommand();
  if (nameCommand == nullptr) {
    return finish(63);
  }
  nameCommand->entity = first;
  nameCommand->persistentId = persistentId;
  nameCommand->type = ComponentEditType::Name;
  nameCommand->beforeExists = false;
  nameCommand->afterExists = true;
  nameCommand->after = added;

  engine::editor::CommandHistory history{};
  history.execute(nameCommand);

  auto *transformCommand = new (std::nothrow) TransformEditCommand();
  if (transformCommand == nullptr) {
    return finish(64);
  }
  transformCommand->entity = first;
  transformCommand->persistentId = persistentId;
  transformCommand->oldTransform = Transform{};
  transformCommand->newTransform.position =
      engine::math::Vec3(1.0F, 2.0F, 3.0F);
  history.execute(transformCommand);

  Transform moved{};
  if (!world->get_transform(first, &moved) ||
      (moved.position.x != 1.0F) || (moved.position.y != 2.0F) ||
      (moved.position.z != 3.0F)) {
    return finish(65);
  }

  if (!world->destroy_entity(first)) {
    return finish(66);
  }
  Transform displaced{};
  displaced.position = engine::math::Vec3(9.0F, 9.0F, 9.0F);
  const Entity second =
      world->create_scene_object_with_persistent_id(persistentId, displaced);
  if ((second == engine::runtime::kInvalidEntity) ||
      (second.generation == first.generation)) {
    return finish(67);
  }

  history.undo();
  Transform reverted{};
  if (!world->get_transform(second, &reverted) ||
      (reverted.position.x != 0.0F) || (reverted.position.y != 0.0F) ||
      (reverted.position.z != 0.0F)) {
    return finish(68);
  }

  history.undo();
  NameComponent afterUndo{};
  if (world->get_name_component(second, &afterUndo)) {
    return finish(69);
  }

  history.redo();
  NameComponent afterRedo{};
  if (!world->get_name_component(second, &afterRedo) ||
      (std::strcmp(afterRedo.name, "Tracked") != 0)) {
    return finish(70);
  }

  return finish(0);
}

/// A reparent command must keep targeting its child across the child being
/// destroyed and re-created under the same persistent id.
int check_reparent_resolves_persistent_id_after_recreate() noexcept {
  using engine::editor::ReparentCommand;
  using engine::runtime::Entity;
  using engine::runtime::Transform;
  using engine::runtime::World;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 80;
  }

  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  const Entity parent = world->create_scene_object();
  const Entity child = world->create_scene_object();
  if ((parent == engine::runtime::kInvalidEntity) ||
      (child == engine::runtime::kInvalidEntity)) {
    return finish(81);
  }
  const engine::runtime::PersistentId parentId = world->persistent_id(parent);
  const engine::runtime::PersistentId childId = world->persistent_id(child);

  auto *command = new (std::nothrow) ReparentCommand();
  if (command == nullptr) {
    return finish(82);
  }
  command->child = child;
  command->childPersistentId = childId;
  command->beforeParentId = engine::runtime::kInvalidPersistentId;
  command->afterParentId = parentId;

  engine::editor::CommandHistory history{};
  history.execute(command);

  Transform linked{};
  if (!world->get_transform(child, &linked) ||
      (linked.parentId != parentId)) {
    return finish(83);
  }

  if (!world->destroy_entity(child)) {
    return finish(84);
  }
  Transform prelinked{};
  prelinked.parentId = parentId;
  const Entity rebuilt =
      world->create_scene_object_with_persistent_id(childId, prelinked);
  if ((rebuilt == engine::runtime::kInvalidEntity) ||
      (rebuilt.generation == child.generation)) {
    return finish(85);
  }

  history.undo();
  Transform unlinked{};
  if (!world->get_transform(rebuilt, &unlinked) ||
      (unlinked.parentId != engine::runtime::kInvalidPersistentId)) {
    return finish(86);
  }

  history.redo();
  Transform relinked{};
  if (!world->get_transform(rebuilt, &relinked) ||
      (relinked.parentId != parentId)) {
    return finish(87);
  }

  return finish(0);
}

/// Entity creation must be undoable and must redo under the same
/// persistent id with its default name intact.
int check_entity_create_undo_redo() noexcept {
  using engine::editor::EntityCreateCommand;
  using engine::runtime::Entity;
  using engine::runtime::NameComponent;
  using engine::runtime::World;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 90;
  }

  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  auto *command = new (std::nothrow) EntityCreateCommand();
  if (command == nullptr) {
    return finish(91);
  }
  engine::editor::CommandHistory history{};
  history.execute(command);

  const engine::runtime::PersistentId persistentId = command->persistentId;
  const Entity created = world->find_entity_by_persistent_id(persistentId);
  if ((created == engine::runtime::kInvalidEntity) ||
      (world->alive_entity_count() != 1U)) {
    return finish(92);
  }

  char expectedName[64] = {};
  std::snprintf(expectedName, sizeof(expectedName), "Entity_%u",
                created.index);
  NameComponent createdName{};
  if (!world->get_name_component(created, &createdName) ||
      (std::strcmp(createdName.name, expectedName) != 0)) {
    return finish(93);
  }

  history.undo();
  if ((world->alive_entity_count() != 0U) ||
      (world->find_entity_by_persistent_id(persistentId) !=
       engine::runtime::kInvalidEntity)) {
    return finish(94);
  }

  history.redo();
  const Entity redone = world->find_entity_by_persistent_id(persistentId);
  if ((redone == engine::runtime::kInvalidEntity) ||
      (world->alive_entity_count() != 1U)) {
    return finish(95);
  }
  NameComponent redoneName{};
  if (!world->get_name_component(redone, &redoneName) ||
      (std::strcmp(redoneName.name, expectedName) != 0)) {
    return finish(96);
  }

  return finish(0);
}

/// Deleting a parented subtree must restore every member, component
/// values, and parent links on undo — under the original persistent ids.
int check_entity_delete_restores_subtree() noexcept {
  using engine::editor::EntityDeleteCommand;
  using engine::runtime::Entity;
  using engine::runtime::MeshComponent;
  using engine::runtime::NameComponent;
  using engine::runtime::Transform;
  using engine::runtime::World;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 100;
  }

  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  const Entity parent = world->create_scene_object();
  if (parent == engine::runtime::kInvalidEntity) {
    return finish(101);
  }
  NameComponent parentName{};
  std::snprintf(parentName.name, sizeof(parentName.name), "%s", "Parent");
  MeshComponent parentMesh{};
  parentMesh.meshAssetId = 42ULL;
  parentMesh.albedo = engine::math::Vec3(0.25F, 0.5F, 0.75F);
  if (!world->add_name_component(parent, parentName) ||
      !world->add_mesh_component(parent, parentMesh)) {
    return finish(102);
  }
  const engine::runtime::PersistentId parentId = world->persistent_id(parent);

  Transform childTransform{};
  childTransform.parentId = parentId;
  childTransform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  const Entity child = world->create_scene_object(childTransform);
  if (child == engine::runtime::kInvalidEntity) {
    return finish(103);
  }
  NameComponent childName{};
  std::snprintf(childName.name, sizeof(childName.name), "%s", "Child");
  if (!world->add_name_component(child, childName)) {
    return finish(104);
  }
  const engine::runtime::PersistentId childId = world->persistent_id(child);

  Transform grandTransform{};
  grandTransform.parentId = childId;
  const Entity grand = world->create_scene_object(grandTransform);
  if (grand == engine::runtime::kInvalidEntity) {
    return finish(105);
  }
  const engine::runtime::PersistentId grandId = world->persistent_id(grand);

  EntityDeleteCommand *const command =
      engine::editor::build_entity_delete_command(parent);
  if ((command == nullptr) || (command->recordCount != 3U)) {
    delete command;
    return finish(106);
  }

  engine::editor::CommandHistory history{};
  history.execute(command);
  if (world->alive_entity_count() != 0U) {
    return finish(107);
  }

  history.undo();
  if (world->alive_entity_count() != 3U) {
    return finish(108);
  }
  const Entity restoredParent = world->find_entity_by_persistent_id(parentId);
  const Entity restoredChild = world->find_entity_by_persistent_id(childId);
  const Entity restoredGrand = world->find_entity_by_persistent_id(grandId);
  if ((restoredParent == engine::runtime::kInvalidEntity) ||
      (restoredChild == engine::runtime::kInvalidEntity) ||
      (restoredGrand == engine::runtime::kInvalidEntity)) {
    return finish(109);
  }

  NameComponent restoredParentName{};
  MeshComponent restoredMesh{};
  if (!world->get_name_component(restoredParent, &restoredParentName) ||
      (std::strcmp(restoredParentName.name, "Parent") != 0) ||
      !world->get_mesh_component(restoredParent, &restoredMesh) ||
      (restoredMesh.meshAssetId != 42ULL) ||
      (restoredMesh.albedo.x != 0.25F) || (restoredMesh.albedo.y != 0.5F) ||
      (restoredMesh.albedo.z != 0.75F)) {
    return finish(110);
  }

  NameComponent restoredChildName{};
  Transform restoredChildTransform{};
  if (!world->get_name_component(restoredChild, &restoredChildName) ||
      (std::strcmp(restoredChildName.name, "Child") != 0) ||
      !world->get_transform(restoredChild, &restoredChildTransform) ||
      (restoredChildTransform.parentId != parentId) ||
      (restoredChildTransform.position.x != 1.0F) ||
      (restoredChildTransform.position.y != 2.0F) ||
      (restoredChildTransform.position.z != 3.0F)) {
    return finish(111);
  }

  Transform restoredGrandTransform{};
  if (!world->get_transform(restoredGrand, &restoredGrandTransform) ||
      (restoredGrandTransform.parentId != childId)) {
    return finish(112);
  }

  history.redo();
  if ((world->alive_entity_count() != 0U) ||
      (world->find_entity_by_persistent_id(parentId) !=
       engine::runtime::kInvalidEntity)) {
    return finish(113);
  }

  history.undo();
  if (world->alive_entity_count() != 3U) {
    return finish(114);
  }
  Transform secondRestore{};
  const Entity childAgain = world->find_entity_by_persistent_id(childId);
  if ((childAgain == engine::runtime::kInvalidEntity) ||
      !world->get_transform(childAgain, &secondRestore) ||
      (secondRestore.parentId != parentId)) {
    return finish(115);
  }

  return finish(0);
}

/// An asset-spawn create command must place the entity at the requested
/// transform with its mesh reference, and restore both on redo.
int check_entity_create_with_mesh_payload() noexcept {
  using engine::editor::EntityCreateCommand;
  using engine::runtime::Entity;
  using engine::runtime::MeshComponent;
  using engine::runtime::Transform;
  using engine::runtime::World;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 130;
  }

  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  auto *command = new (std::nothrow) EntityCreateCommand();
  if (command == nullptr) {
    return finish(131);
  }
  command->transform.position = engine::math::Vec3(4.0F, 0.0F, -2.0F);
  std::snprintf(command->name.name, sizeof(command->name.name), "%s", "rock");
  command->hasMesh = true;
  command->mesh.meshAssetId = 777ULL;

  engine::editor::CommandHistory history{};
  history.execute(command);

  const engine::runtime::PersistentId persistentId = command->persistentId;
  const Entity created = world->find_entity_by_persistent_id(persistentId);
  if (created == engine::runtime::kInvalidEntity) {
    return finish(132);
  }
  Transform placed{};
  MeshComponent meshComponent{};
  if (!world->get_transform(created, &placed) ||
      (placed.position.x != 4.0F) || (placed.position.y != 0.0F) ||
      (placed.position.z != -2.0F) ||
      !world->get_mesh_component(created, &meshComponent) ||
      (meshComponent.meshAssetId != 777ULL)) {
    return finish(133);
  }

  history.undo();
  if (world->alive_entity_count() != 0U) {
    return finish(134);
  }

  history.redo();
  const Entity redone = world->find_entity_by_persistent_id(persistentId);
  Transform redonePlaced{};
  MeshComponent redoneMesh{};
  if ((redone == engine::runtime::kInvalidEntity) ||
      !world->get_transform(redone, &redonePlaced) ||
      (redonePlaced.position.x != 4.0F) ||
      !world->get_mesh_component(redone, &redoneMesh) ||
      (redoneMesh.meshAssetId != 777ULL)) {
    return finish(135);
  }

  return finish(0);
}

/// A create-then-edit history chain must survive full undo and redo even
/// though redo re-creates the entity with a fresh generation.
int check_create_edit_chain_survives_undo_redo() noexcept {
  using engine::editor::ComponentEditCommand;
  using engine::editor::ComponentEditSnapshot;
  using engine::editor::ComponentEditType;
  using engine::editor::EntityCreateCommand;
  using engine::runtime::Entity;
  using engine::runtime::NameComponent;
  using engine::runtime::World;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 120;
  }

  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  auto *createCommand = new (std::nothrow) EntityCreateCommand();
  if (createCommand == nullptr) {
    return finish(121);
  }
  engine::editor::CommandHistory history{};
  history.execute(createCommand);

  const engine::runtime::PersistentId persistentId =
      createCommand->persistentId;
  const Entity created = world->find_entity_by_persistent_id(persistentId);
  if (created == engine::runtime::kInvalidEntity) {
    return finish(122);
  }

  ComponentEditSnapshot before{};
  if (!engine::editor::capture_component_snapshot(ComponentEditType::Name,
                                                  created, &before)) {
    return finish(123);
  }
  ComponentEditSnapshot after{};
  std::snprintf(after.name.name, sizeof(after.name.name), "%s", "Renamed");
  auto *renameCommand = new (std::nothrow) ComponentEditCommand();
  if (renameCommand == nullptr) {
    return finish(124);
  }
  renameCommand->entity = created;
  renameCommand->persistentId = persistentId;
  renameCommand->type = ComponentEditType::Name;
  renameCommand->beforeExists = true;
  renameCommand->before = before;
  renameCommand->afterExists = true;
  renameCommand->after = after;
  history.execute(renameCommand);

  history.undo();
  history.undo();
  if (world->alive_entity_count() != 0U) {
    return finish(125);
  }

  history.redo();
  history.redo();
  const Entity redone = world->find_entity_by_persistent_id(persistentId);
  NameComponent finalName{};
  if ((redone == engine::runtime::kInvalidEntity) ||
      (world->alive_entity_count() != 1U) ||
      !world->get_name_component(redone, &finalName) ||
      (std::strcmp(finalName.name, "Renamed") != 0)) {
    return finish(126);
  }

  return finish(0);
}

} // namespace

static_assert(!std::is_copy_constructible_v<engine::editor::CommandHistory>);
static_assert(!std::is_copy_assignable_v<engine::editor::CommandHistory>);
static_assert(!std::is_move_constructible_v<engine::editor::CommandHistory>);
static_assert(!std::is_move_assignable_v<engine::editor::CommandHistory>);

int main() {
  int result = check_parented_gizmo_world_to_local_conversion();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_destructor_releases_commands();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_redo_entries_are_released();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_capacity_eviction_releases_oldest_command();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_undo_redo_dispatch();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_component_command_rejects_recycled_entity();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_commands_resolve_persistent_id_after_recreate();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_reparent_resolves_persistent_id_after_recreate();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_entity_create_undo_redo();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_entity_delete_restores_subtree();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_entity_create_with_mesh_payload();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_create_edit_chain_survives_undo_redo();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  std::printf("command_history_test: all tests passed\n");
  return 0;
}
