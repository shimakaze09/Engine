// Verifies deleting the editor selection: the Delete key, through the
// production shortcut dispatcher on a headless ImGui frame, removes every
// selected entity as one undoable command. A selected entity whose
// ancestor is also selected goes with that ancestor rather than being
// captured twice, and one undo restores every member under its original
// persistent id and parent. Delete is disabled with nothing selected and
// during play, and a stale root refuses the whole command, leaving the
// world untouched.

#include "editor_commands.h"
#include "editor_session.h"
#include "editor_shortcuts.h"

#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <memory>
#include <new>

namespace {

using namespace engine::editor;
using namespace engine::runtime;

void run_frame() noexcept {
  ImGui::NewFrame();
  dispatch_editor_shortcuts();
  ImGui::Render();
}

/// Presses and releases `key` over two frames, as the user would.
void tap(ImGuiKey key) noexcept {
  ImGui::GetIO().AddKeyEvent(key, true);
  run_frame();
  ImGui::GetIO().AddKeyEvent(key, false);
  run_frame();
}

Entity make(World &world, Entity parent) noexcept {
  Transform transform{};
  if (parent != kInvalidEntity) {
    transform.parentId = world.persistent_id(parent);
  }
  return world.create_scene_object(transform);
}

PersistentId parent_id(const World &world, PersistentId child) noexcept {
  Transform transform{};
  const Entity entity = world.find_entity_by_persistent_id(child);
  return world.get_transform(entity, &transform) ? transform.parentId
                                                 : kInvalidPersistentId;
}

bool alive(const World &world, PersistentId id) noexcept {
  return world.find_entity_by_persistent_id(id) != kInvalidEntity;
}

void check_forest_delete_and_undo(engine::tests::TestContext &t,
                                  World &world) noexcept {
  // root
  //   child
  //     grandchild
  //   sibling
  // loner
  const Entity root = make(world, kInvalidEntity);
  const Entity child = make(world, root);
  const Entity grandchild = make(world, child);
  const Entity sibling = make(world, root);
  const Entity loner = make(world, kInvalidEntity);
  const PersistentId rootId = world.persistent_id(root);
  const PersistentId childId = world.persistent_id(child);
  const PersistentId grandchildId = world.persistent_id(grandchild);
  const PersistentId siblingId = world.persistent_id(sibling);
  const PersistentId lonerId = world.persistent_id(loner);

  // The grandchild is listed, and so is its parent: it goes once, with
  // the child.
  select_entity(grandchild, false);
  select_entity(child, true);
  select_entity(loner, true);
  t.check(editor_session().selectedEntityCount == 3U, "three selected");
  const std::size_t before = world.alive_entity_count();

  // On base the Delete key had no handler at all.
  tap(ImGuiKey_Delete);
  t.check(world.alive_entity_count() == before - 3U,
          "Delete removes the selection and its subtrees");
  t.check(!alive(world, childId) && !alive(world, grandchildId) &&
              !alive(world, lonerId),
          "the selected entities are gone");
  t.check(alive(world, rootId) && alive(world, siblingId),
          "unselected entities stay");
  t.check((editor_session().selectedEntityCount == 0U) &&
              (selected_entity() == kInvalidEntity),
          "the selection is cleared");

  // One undo restores all of it, identities and parents intact.
  t.check(editor_history_can_undo(), "the delete is undoable");
  editor_history_undo();
  t.check(world.alive_entity_count() == before, "one undo restores all");
  t.check(alive(world, childId) && alive(world, grandchildId) &&
              alive(world, lonerId),
          "every member returns under its persistent id");
  t.check((parent_id(world, childId) == rootId) &&
              (parent_id(world, grandchildId) == childId) &&
              (parent_id(world, lonerId) == kInvalidPersistentId),
          "parent links survive the round trip");

  // Redo deletes them again, as one step.
  editor_history_redo();
  t.check(world.alive_entity_count() == before - 3U, "redo deletes again");
  editor_history_undo();
  t.check(world.alive_entity_count() == before, "undo restores again");
}

void check_parent_and_child_listed(engine::tests::TestContext &t,
                                   World &world) noexcept {
  const Entity parent = make(world, kInvalidEntity);
  const Entity child = make(world, parent);
  const Entity grandchild = make(world, child);
  // Child listed before its ancestor, and one entry repeated.
  const Entity listed[] = {grandchild, child, parent, child};
  EntityDeleteCommand *command = build_entity_delete_command(listed, 4U);
  t.check((command != nullptr) && (command->rootCount == 1U) &&
              (command->recordCount == 3U),
          "a listed ancestor absorbs its listed descendants, once each");
  delete command;
}

void check_stale_root_refuses(engine::tests::TestContext &t,
                              World &world) noexcept {
  const Entity a = make(world, kInvalidEntity);
  const Entity b = make(world, kInvalidEntity);
  const Entity pair[] = {a, b};
  EntityDeleteCommand *command = build_entity_delete_command(pair, 2U);
  t.check((command != nullptr) && (command->rootCount == 2U), "two roots");
  t.check(world.destroy_entity(b), "one root goes behind the command");
  const std::size_t before = world.alive_entity_count();
  t.check((command != nullptr) && !command->execute(),
          "a stale root refuses the whole command");
  t.check(world.is_alive(a) && (world.alive_entity_count() == before),
          "nothing was deleted");
  delete command;
}

void check_disabled_states(engine::tests::TestContext &t,
                           World &world) noexcept {
  clear_entity_selection();
  t.check(!editor_action_enabled(EditorAction::Delete),
          "Delete is disabled with nothing selected");
  std::size_t before = world.alive_entity_count();
  tap(ImGuiKey_Delete);
  t.check(world.alive_entity_count() == before,
          "the key does nothing with nothing selected");

  const Entity entity = make(world, kInvalidEntity);
  select_entity(entity, false);
  t.check(editor_action_enabled(EditorAction::Delete),
          "Delete is enabled with a selection");
  editor_session().playState = PlayState::Playing;
  t.check(!editor_action_enabled(EditorAction::Delete),
          "Delete is disabled during play");
  before = world.alive_entity_count();
  tap(ImGuiKey_Delete);
  t.check(world.alive_entity_count() == before,
          "the key does nothing during play");
  editor_session().playState = PlayState::Stopped;
  clear_entity_selection();
}

} // namespace

int main() {
  engine::tests::TestContext t;
  static_cast<void>(engine::core::initialize_logging());

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 1.0F / 60.0F;
  io.IniFilename = nullptr;
  io.ConfigInputTrickleEventQueue = false;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }
  editor_set_world(world.get());

  check_forest_delete_and_undo(t, *world);
  check_parent_and_child_listed(t, *world);
  check_stale_root_refuses(t, *world);
  check_disabled_states(t, *world);

  editor_set_world(nullptr);
  ImGui::DestroyContext();
  engine::core::shutdown_logging();
  return t.finish("editor_delete_selection");
}
