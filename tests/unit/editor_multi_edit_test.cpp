// Verifies the multi-entity Inspector support declared in
// editor_multi_edit.h (issue #159): common-component computation across a
// selection, per-field mixed-value detection that never conflates "mixed"
// with "one entity's value", a batch field edit applying to every selected
// entity as ONE undoable command that leaves sibling mixed fields alone,
// atomic rollback when one selected entity can no longer accept the write,
// and batch component removal as one undoable command too.

#include "editor_commands.h"
#include "editor_multi_edit.h"
#include "editor_session.h"
#include "engine/core/reflect.h"
#include "engine/runtime/world.h"

#include <array>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using namespace engine::editor;
using engine::runtime::Entity;
using engine::runtime::RigidBody;
using engine::runtime::World;

/// Binds a fresh world to the editor session and clears selection/history
/// state on destruction so tests never leak into each other.
struct SessionWorldScope final {
  World *previousWorld = nullptr;

  explicit SessionWorldScope(World *world) noexcept {
    EditorSession &session = editor_session();
    previousWorld = session.world;
    session.world = world;
    session.playState = PlayState::Stopped;
    session.worldRestoreFailed = false;
  }

  ~SessionWorldScope() noexcept {
    EditorSession &session = editor_session();
    clear_entity_selection();
    session.commandHistory.clear();
    session.world = previousWorld;
  }
};

Entity make_rigid_body_entity(World &world, float inverseMass,
                              float inverseInertia) noexcept {
  const Entity entity = world.create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return engine::runtime::kInvalidEntity;
  }
  RigidBody body{};
  body.inverseMass = inverseMass;
  body.inverseInertia = inverseInertia;
  if (!world.add_rigid_body(entity, body)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

const engine::core::TypeField *rigid_body_field(const char *name) noexcept {
  const engine::core::TypeDescriptor *desc =
      engine::core::global_type_registry().find_type(
          "engine::runtime::RigidBody");
  return (desc != nullptr) ? desc->find_field(name) : nullptr;
}

/// EXPECTATION: a component present on every selected entity is common; one
/// absent on any selected entity is not. A field that disagrees across the
/// selection is mixed; a field every entity agrees on is not, even though
/// the selection itself is mixed on a different field of the same
/// component.
int check_common_components_and_mixed_fields() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  SessionWorldScope scope(world.get());

  const Entity first = make_rigid_body_entity(*world, 1.0F, 5.0F);
  const Entity second = make_rigid_body_entity(*world, 2.0F, 5.0F);
  const Entity third = make_rigid_body_entity(*world, 3.0F, 5.0F);
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity) ||
      (third == engine::runtime::kInvalidEntity)) {
    return 2;
  }
  // Only `first` gets a Collider, so Collider must not read as common.
  engine::runtime::Collider collider{};
  if (!world->add_collider(first, collider)) {
    return 3;
  }

  select_entity(first, false);
  select_entity(second, true);
  select_entity(third, true);
  if (editor_session().selectedEntityCount != 3U) {
    return 4;
  }

  std::array<bool, kComponentEditTypeCount> common{};
  compute_selection_common_components(&common);
  if (!common[static_cast<std::size_t>(ComponentEditType::RigidBody)]) {
    return 5;
  }
  if (common[static_cast<std::size_t>(ComponentEditType::Collider)]) {
    return 6;
  }

  const engine::core::TypeField *massField = rigid_body_field("inverseMass");
  const engine::core::TypeField *inertiaField = rigid_body_field("inverseInertia");
  if ((massField == nullptr) || (inertiaField == nullptr)) {
    return 7;
  }
  if (!selection_field_is_mixed(ComponentEditType::RigidBody,
                                massField->offset, massField->size)) {
    return 8; // 1.0/2.0/3.0 must read as mixed
  }
  if (selection_field_is_mixed(ComponentEditType::RigidBody,
                               inertiaField->offset, inertiaField->size)) {
    return 9; // all three share 5.0 -- must not read as mixed
  }

  return 0;
}

/// EXPECTATION: editing one mixed field applies to every selected entity as
/// ONE undoable command, and a sibling field left untouched at each entity
/// keeps its own (possibly still-mixed) per-entity value rather than being
/// clobbered to the representative entity's value.
int check_batch_field_edit_single_command_preserves_sibling_field() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 20;
  }
  SessionWorldScope scope(world.get());

  const Entity first = make_rigid_body_entity(*world, 1.0F, 10.0F);
  const Entity second = make_rigid_body_entity(*world, 2.0F, 20.0F);
  const Entity third = make_rigid_body_entity(*world, 3.0F, 30.0F);
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity) ||
      (third == engine::runtime::kInvalidEntity)) {
    return 21;
  }
  select_entity(first, false);
  select_entity(second, true);
  select_entity(third, true);

  const engine::core::TypeField *massField = rigid_body_field("inverseMass");
  if (massField == nullptr) {
    return 22;
  }

  ComponentEditSnapshot representative{};
  if (!selection_representative_component(ComponentEditType::RigidBody,
                                          &representative)) {
    return 23;
  }
  representative.rigidBody.inverseMass = 42.0F;

  if (editor_session().commandHistory.can_undo()) {
    return 24;
  }
  if (!apply_multi_field_edit(ComponentEditType::RigidBody, massField->offset,
                              massField->size, representative)) {
    return 25;
  }
  if (!editor_session().commandHistory.can_undo()) {
    return 26; // exactly one history entry for the whole batch
  }

  RigidBody a{};
  RigidBody b{};
  RigidBody c{};
  if (!world->get_rigid_body(first, &a) || !world->get_rigid_body(second, &b) ||
      !world->get_rigid_body(third, &c)) {
    return 27;
  }
  if ((a.inverseMass != 42.0F) || (b.inverseMass != 42.0F) ||
      (c.inverseMass != 42.0F)) {
    return 28; // the edited field must land uniformly on every entity
  }
  if ((a.inverseInertia != 10.0F) || (b.inverseInertia != 20.0F) ||
      (c.inverseInertia != 30.0F)) {
    return 29; // the untouched, still-mixed sibling field must survive
  }

  editor_session().commandHistory.undo();
  RigidBody undoneA{};
  RigidBody undoneB{};
  RigidBody undoneC{};
  if (!world->get_rigid_body(first, &undoneA) ||
      !world->get_rigid_body(second, &undoneB) ||
      !world->get_rigid_body(third, &undoneC)) {
    return 30;
  }
  if ((undoneA.inverseMass != 1.0F) || (undoneB.inverseMass != 2.0F) ||
      (undoneC.inverseMass != 3.0F)) {
    return 31; // undo restores each entity's own original value
  }

  editor_session().commandHistory.redo();
  RigidBody redoneA{};
  if (!world->get_rigid_body(first, &redoneA) ||
      (redoneA.inverseMass != 42.0F)) {
    return 32;
  }

  return 0;
}

/// EXPECTATION: if any selected entity can no longer accept the write (here:
/// destroyed after selection), the whole batch is rolled back atomically --
/// entities already touched this call revert to their pre-edit value, and
/// no history entry is pushed.
int check_batch_edit_rolls_back_on_partial_failure() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 40;
  }
  SessionWorldScope scope(world.get());

  const Entity first = make_rigid_body_entity(*world, 1.0F, 10.0F);
  const Entity second = make_rigid_body_entity(*world, 2.0F, 20.0F);
  const Entity third = make_rigid_body_entity(*world, 3.0F, 30.0F);
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity) ||
      (third == engine::runtime::kInvalidEntity)) {
    return 41;
  }
  select_entity(first, false);
  select_entity(second, true);
  select_entity(third, true);

  // Destroy the last selected entity without pruning the selection, so
  // apply_multi_field_edit encounters a dead entity mid-batch.
  if (!world->destroy_entity(third)) {
    return 42;
  }

  const engine::core::TypeField *massField = rigid_body_field("inverseMass");
  if (massField == nullptr) {
    return 43;
  }
  ComponentEditSnapshot representative{};
  representative.rigidBody.inverseMass = 99.0F;

  if (apply_multi_field_edit(ComponentEditType::RigidBody, massField->offset,
                             massField->size, representative)) {
    return 44; // must fail: not every selected entity is alive
  }
  if (editor_session().commandHistory.can_undo()) {
    return 45; // a rolled-back batch must never reach history
  }

  RigidBody a{};
  RigidBody b{};
  if (!world->get_rigid_body(first, &a) || !world->get_rigid_body(second, &b)) {
    return 46;
  }
  if ((a.inverseMass != 1.0F) || (b.inverseMass != 2.0F)) {
    return 47; // entities touched before the failure must be rolled back
  }

  return 0;
}

/// EXPECTATION: removing a common component from every selected entity is
/// one undoable command; undo restores the component (with its original
/// per-entity values) on every entity, not just the representative one.
int check_batch_remove_single_command_undo_redo() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 60;
  }
  SessionWorldScope scope(world.get());

  const Entity first = make_rigid_body_entity(*world, 1.0F, 10.0F);
  const Entity second = make_rigid_body_entity(*world, 2.0F, 20.0F);
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity)) {
    return 61;
  }
  select_entity(first, false);
  select_entity(second, true);

  if (!apply_multi_component_remove(ComponentEditType::RigidBody)) {
    return 62;
  }
  if (!editor_session().commandHistory.can_undo()) {
    return 63; // one history entry for the whole batch removal
  }
  RigidBody stillThere{};
  if (world->get_rigid_body(first, &stillThere) ||
      world->get_rigid_body(second, &stillThere)) {
    return 64; // both entities must have lost the component
  }

  editor_session().commandHistory.undo();
  RigidBody restoredFirst{};
  RigidBody restoredSecond{};
  if (!world->get_rigid_body(first, &restoredFirst) ||
      !world->get_rigid_body(second, &restoredSecond)) {
    return 65;
  }
  if ((restoredFirst.inverseMass != 1.0F) ||
      (restoredSecond.inverseMass != 2.0F)) {
    return 66; // each entity's own original value, not one shared value
  }

  editor_session().commandHistory.redo();
  if (world->get_rigid_body(first, &restoredFirst) ||
      world->get_rigid_body(second, &restoredSecond)) {
    return 67;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  struct NamedCheck {
    const char *name;
    int (*fn)();
  };
  const NamedCheck checks[] = {
      {"check_common_components_and_mixed_fields",
       &check_common_components_and_mixed_fields},
      {"check_batch_field_edit_single_command_preserves_sibling_field",
       &check_batch_field_edit_single_command_preserves_sibling_field},
      {"check_batch_edit_rolls_back_on_partial_failure",
       &check_batch_edit_rolls_back_on_partial_failure},
      {"check_batch_remove_single_command_undo_redo",
       &check_batch_remove_single_command_undo_redo},
  };

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_multi_edit_test: %s failed: %d\n",
                   check.name, result);
      return result;
    }
  }

  std::printf("editor_multi_edit_test: all tests passed\n");
  return 0;
}
