// Verifies the editor's entity clipboard through its production entry
// points. Paste creates fresh copies of the copied forest beside the
// originals, in one undoable step. Paste As Child puts them under a new
// parent while keeping their world pose. A paste into another document
// lands at the root and clears references that named entities of the old
// one, while references inside the pasted set still point at the new
// copies. Copy works during play, and the copy pastes after Stop.

#include "editor_commands.h"
#include "editor_entity_clipboard.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "editor_shortcuts.h"

#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

using namespace engine::editor;
using namespace engine::runtime;

Entity make(World &world, const char *name, Entity parent,
            const engine::math::Vec3 &position) noexcept {
  Transform local{};
  local.position = position;
  if (parent != kInvalidEntity) {
    local.parentId = world.persistent_id(parent);
  }
  const Entity entity = world.create_scene_object(local);
  NameComponent component{};
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  return world.add_name_component(entity, component) ? entity : kInvalidEntity;
}

Transform local_of(const World &world, Entity entity) noexcept {
  Transform transform{};
  static_cast<void>(world.get_transform(entity, &transform));
  return transform;
}

/// The one entity named `name` whose parent is `parentId`.
Entity find_under(World &world, const char *name,
                  PersistentId parentId) noexcept {
  Entity found = kInvalidEntity;
  world.for_each_alive([&](Entity entity) {
    NameComponent component{};
    if (world.get_name_component(entity, &component) &&
        (std::strcmp(component.name, name) == 0) &&
        (local_of(world, entity).parentId == parentId)) {
      found = entity;
    }
  });
  return found;
}

void check_paste_beside_original(engine::tests::TestContext &t,
                                 World &world) noexcept {
  const Entity group = make(world, "Group", kInvalidEntity,
                            engine::math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity crate =
      make(world, "Crate", group, engine::math::Vec3(1.0F, 0.0F, 0.0F));
  const Entity lid =
      make(world, "Lid", crate, engine::math::Vec3(0.0F, 1.0F, 0.0F));
  select_entity(crate, false);
  t.check(run_editor_action(EditorAction::Copy), "copy the crate");
  const std::size_t before = world.alive_entity_count();
  t.check(run_editor_action(EditorAction::Paste), "paste it");
  t.check(world.alive_entity_count() == before + 2U,
          "the crate and its lid are pasted");
  const Entity pasted =
      find_under(world, "Crate (2)", world.persistent_id(group));
  t.check(pasted != kInvalidEntity,
          "the paste sits beside the original, under the same parent");
  t.check((find_under(world, "Lid", world.persistent_id(pasted)) !=
           kInvalidEntity) &&
              (local_of(world, lid).parentId == world.persistent_id(crate)),
          "the pasted lid hangs under the pasted crate; the original's "
          "stays put");
  t.check(is_entity_selected(pasted), "the pasted root is selected");
  editor_history_undo();
  t.check(world.alive_entity_count() == before, "one undo removes the paste");
  editor_history_redo();
  t.check(world.alive_entity_count() == before + 2U, "redo restores it");
}

void check_paste_as_child(engine::tests::TestContext &t,
                          World &world) noexcept {
  const Entity shelf = make(world, "Shelf", kInvalidEntity,
                            engine::math::Vec3(10.0F, 0.0F, 0.0F));
  const Entity box =
      make(world, "Box", kInvalidEntity, engine::math::Vec3(1.0F, 2.0F, 3.0F));
  select_entity(box, false);
  t.check(run_editor_action(EditorAction::Copy), "copy the box");
  select_entity(shelf, false);
  t.check(run_editor_action(EditorAction::PasteAsChild),
          "paste it as the shelf's child");
  const Entity child = find_under(world, "Box (2)", world.persistent_id(shelf));
  t.check(child != kInvalidEntity, "the box is pasted under the shelf");
  // Keeping the world pose makes the local offset world minus the
  // parent's: (1,2,3) - (10,0,0). Decomposing the product of an inverse
  // leaves a few ulps on values this size.
  const Transform local = local_of(world, child);
  constexpr float kTolerance = 1.0e-5F;
  t.check((std::fabs(local.position.x + 9.0F) <= kTolerance) &&
              (std::fabs(local.position.y - 2.0F) <= kTolerance) &&
              (std::fabs(local.position.z - 3.0F) <= kTolerance),
          "the pasted child keeps the box's world position");
}

void check_other_document(engine::tests::TestContext &t,
                          World &world) noexcept {
  t.check(perform_scene_new(), "a fresh document");
  const Entity rig =
      make(world, "Rig", kInvalidEntity, engine::math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity camera =
      make(world, "Camera", rig, engine::math::Vec3(0.0F, 0.0F, 0.0F));
  const Entity screen =
      make(world, "Screen", rig, engine::math::Vec3(4.0F, 0.0F, 0.0F));
  const Entity stranger = make(world, "Stranger", kInvalidEntity,
                               engine::math::Vec3(0.0F, 0.0F, 0.0F));
  MeshComponent inside{};
  inside.sceneCaptureSourceId = world.persistent_id(camera);
  MeshComponent outside{};
  outside.sceneCaptureSourceId = world.persistent_id(stranger);
  const Entity poster =
      make(world, "Poster", rig, engine::math::Vec3(0.0F, 3.0F, 0.0F));
  t.check(world.add_mesh_component(screen, inside) &&
              world.add_mesh_component(poster, outside),
          "one reference inside the rig, one out of it");
  select_entity(rig, false);
  t.check(entity_clipboard_copy(), "copy the rig");

  t.check(perform_scene_new(), "another document");
  // Something in the new document under the stranger's old id, so a kept
  // reference would name it.
  const Entity squatter = make(world, "Squatter", kInvalidEntity,
                               engine::math::Vec3(0.0F, 0.0F, 0.0F));
  static_cast<void>(squatter);
  t.check(execute_entity_paste(kInvalidEntity), "paste into it");
  const Entity pastedRig = find_under(world, "Rig", kInvalidPersistentId);
  const PersistentId rigId = world.persistent_id(pastedRig);
  const Entity pastedCamera = find_under(world, "Camera", rigId);
  MeshComponent mesh{};
  t.check(
      (pastedCamera != kInvalidEntity) &&
          world.get_mesh_component(find_under(world, "Screen", rigId), &mesh) &&
          (mesh.sceneCaptureSourceId == world.persistent_id(pastedCamera)),
      "a reference inside the pasted set points at the new copy");
  t.check(world.get_mesh_component(find_under(world, "Poster", rigId), &mesh) &&
              (mesh.sceneCaptureSourceId == 0U),
          "a reference out of the set, from another document, is cleared");
}

void check_copy_in_play(engine::tests::TestContext &t, World &world) noexcept {
  t.check(perform_scene_new(), "a fresh document");
  const Entity actor = make(world, "Actor", kInvalidEntity,
                            engine::math::Vec3(0.0F, 0.0F, 0.0F));
  select_entity(actor, false);
  editor_session().playState = PlayState::Playing;
  t.check(editor_action_enabled(EditorAction::Copy) &&
              run_editor_action(EditorAction::Copy),
          "copy works during play");
  t.check(!editor_action_enabled(EditorAction::Paste) &&
              !execute_entity_paste(kInvalidEntity),
          "paste waits for Stop");
  editor_session().playState = PlayState::Stopped;
  const std::size_t before = world.alive_entity_count();
  t.check(execute_entity_paste(kInvalidEntity) &&
              (world.alive_entity_count() == before + 1U),
          "after Stop the copy pastes");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  static_cast<void>(engine::core::initialize_logging());
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }
  editor_set_world(world.get());
  entity_clipboard_clear();

  check_paste_beside_original(t, *world);
  check_paste_as_child(t, *world);
  check_other_document(t, *world);
  check_copy_in_play(t, *world);

  entity_clipboard_clear();
  editor_set_world(nullptr);
  engine::core::shutdown_logging();
  return t.finish("editor_entity_clipboard");
}
