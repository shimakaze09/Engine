// Verifies the Inspector's component operations through their production
// entry points. Reset returns a component to its Add Component value (a
// Transform keeping its parent) over one or many entities as one undoable
// command, and is refused for Name and Script. Copy, Paste Component
// Values and Paste Component As New carry values between entities as one
// command each: a pasted Transform keeps the target's parent, a pasted
// Animation starts from its controller's entry state, and an entity
// reference copied in another scene document is cleared. Copy works
// during play; paste does not.

#include "editor_commands.h"
#include "editor_component_ops.h"
#include "editor_scene_document.h"
#include "editor_session.h"

#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <memory>
#include <new>

namespace {

using namespace engine::editor;
using namespace engine::runtime;

ComponentEditSnapshot get(Entity entity, ComponentEditType type,
                          bool *outExists = nullptr) noexcept {
  ComponentEditSnapshot snapshot{};
  const bool exists = capture_component_snapshot(type, entity, &snapshot);
  if (outExists != nullptr) {
    *outExists = exists;
  }
  return snapshot;
}

bool has(Entity entity, ComponentEditType type) noexcept {
  bool exists = false;
  static_cast<void>(get(entity, type, &exists));
  return exists;
}

bool set_collider(Entity entity, float halfExtent) noexcept {
  ComponentEditSnapshot snapshot =
      default_component_snapshot(entity, ComponentEditType::Collider);
  snapshot.collider.halfExtents =
      engine::math::Vec3(halfExtent, halfExtent, halfExtent);
  return apply_component_snapshot(ComponentEditType::Collider, entity, true,
                                  snapshot);
}

float collider_extent(Entity entity) noexcept {
  return get(entity, ComponentEditType::Collider).collider.halfExtents.x;
}

void check_reset(engine::tests::TestContext &t, World &world) noexcept {
  const Entity parent = world.create_scene_object();
  Transform local{};
  local.parentId = world.persistent_id(parent);
  local.position = engine::math::Vec3(5.0F, 1.0F, -2.0F);
  local.scale = engine::math::Vec3(2.0F, 2.0F, 2.0F);
  const Entity child = world.create_scene_object(local);
  t.check(execute_component_reset(&child, 1U, ComponentEditType::Transform),
          "reset a Transform");
  const Transform reset = get(child, ComponentEditType::Transform).transform;
  t.check((reset.position.x == 0.0F) && (reset.position.y == 0.0F) &&
              (reset.position.z == 0.0F) && (reset.scale.x == 1.0F) &&
              (reset.parentId == world.persistent_id(parent)),
          "a reset Transform is at the origin, unit scale, same parent");
  editor_history_undo();
  t.check(get(child, ComponentEditType::Transform).transform.position.x == 5.0F,
          "undo restores the Transform");

  // Several entities at once, one of them without the component: one
  // command, which undo reverts whole.
  const Entity a = world.create_scene_object();
  const Entity b = world.create_scene_object();
  const Entity c = world.create_scene_object();
  t.check(set_collider(a, 3.0F) && set_collider(b, 4.0F), "two colliders");
  const Entity targets[] = {a, b, c};
  t.check(execute_component_reset(targets, 3U, ComponentEditType::Collider),
          "reset colliders over a selection");
  t.check((collider_extent(a) == 0.5F) && (collider_extent(b) == 0.5F) &&
              !has(c, ComponentEditType::Collider),
          "each carrier resets; the entity without one gains nothing");
  editor_history_undo();
  t.check((collider_extent(a) == 3.0F) && (collider_extent(b) == 4.0F),
          "one undo reverts the whole reset");

  t.check(!component_reset_available(ComponentEditType::Name) &&
              !component_reset_available(ComponentEditType::Script) &&
              !execute_component_reset(&a, 1U, ComponentEditType::Name),
          "Name and Script have no Reset");
}

void check_copy_paste(engine::tests::TestContext &t, World &world) noexcept {
  const Entity source = world.create_scene_object();
  const Entity target = world.create_scene_object();
  const Entity bare = world.create_scene_object();
  t.check(set_collider(source, 2.0F) && set_collider(target, 0.5F),
          "colliders to copy between");
  t.check(component_clipboard_copy(source, ComponentEditType::Collider),
          "copy a collider");
  ComponentEditType held = ComponentEditType::Transform;
  t.check(component_clipboard_type(&held) &&
              (held == ComponentEditType::Collider),
          "the clipboard holds a collider");

  const Entity both[] = {target, bare};
  t.check(execute_component_paste_values(both, 2U), "paste values");
  t.check((collider_extent(target) == 2.0F) &&
              !has(bare, ComponentEditType::Collider),
          "values land only where the component exists");
  t.check(execute_component_paste_as_new(both, 2U), "paste as new");
  t.check(has(bare, ComponentEditType::Collider) &&
              (collider_extent(bare) == 2.0F),
          "as-new adds it where it was missing");
  editor_history_undo();
  t.check(!has(bare, ComponentEditType::Collider), "undo removes it again");
  editor_history_undo();
  t.check(collider_extent(target) == 0.5F, "undo restores the values");

  // A pasted Transform keeps the target's own parent.
  const Entity parent = world.create_scene_object();
  Transform moved{};
  moved.position = engine::math::Vec3(7.0F, 0.0F, 0.0F);
  const Entity mover = world.create_scene_object(moved);
  Transform childLocal{};
  childLocal.parentId = world.persistent_id(parent);
  const Entity child = world.create_scene_object(childLocal);
  t.check(component_clipboard_copy(mover, ComponentEditType::Transform) &&
              execute_component_paste_values(&child, 1U),
          "paste a Transform");
  const Transform pasted = get(child, ComponentEditType::Transform).transform;
  t.check((pasted.position.x == 7.0F) &&
              (pasted.parentId == world.persistent_id(parent)),
          "the pasted Transform keeps the target's parent");

  // An Animation's state-machine position is the source's runtime state.
  ComponentEditSnapshot anim =
      default_component_snapshot(source, ComponentEditType::Animation);
  anim.animation.currentState = 3U;
  anim.animation.stateTime = 1.5F;
  t.check(apply_component_snapshot(ComponentEditType::Animation, source, true,
                                   anim),
          "an animation mid-state");
  t.check(component_clipboard_copy(source, ComponentEditType::Animation) &&
              execute_component_paste_as_new(&target, 1U),
          "paste an Animation as new");
  const ComponentEditSnapshot got = get(target, ComponentEditType::Animation);
  t.check((got.animation.currentState == 0U) &&
              (got.animation.stateTime == 0.0F),
          "the pasted Animation starts at its entry state");
}

void check_documents_and_play(engine::tests::TestContext &t,
                              World &world) noexcept {
  // A capture-source reference survives a paste within its document and
  // is cleared across documents.
  const Entity capture = world.create_scene_object();
  const Entity source = world.create_scene_object();
  ComponentEditSnapshot mesh =
      default_component_snapshot(source, ComponentEditType::Mesh);
  mesh.mesh.sceneCaptureSourceId = world.persistent_id(capture);
  t.check(apply_component_snapshot(ComponentEditType::Mesh, source, true, mesh),
          "a mesh that shows a capture");
  t.check(component_clipboard_copy(source, ComponentEditType::Mesh),
          "copy the mesh");
  const Entity sameDoc = world.create_scene_object();
  t.check(
      execute_component_paste_as_new(&sameDoc, 1U) &&
          (get(sameDoc, ComponentEditType::Mesh).mesh.sceneCaptureSourceId ==
           world.persistent_id(capture)),
      "within one document the reference is kept");

  t.check(perform_scene_new(), "a new document");
  const Entity otherDoc = world.create_scene_object();
  t.check(
      execute_component_paste_as_new(&otherDoc, 1U) &&
          (get(otherDoc, ComponentEditType::Mesh).mesh.sceneCaptureSourceId ==
           0U),
      "in another document the reference is cleared");

  // Copy reads during play; paste waits for an editable world.
  const Entity player = world.create_scene_object();
  t.check(set_collider(player, 6.0F), "a collider to copy in play");
  editor_session().playState = PlayState::Playing;
  t.check(component_clipboard_copy(player, ComponentEditType::Collider),
          "copy works during play");
  const Entity receiver = world.create_scene_object();
  t.check(!execute_component_paste_as_new(&receiver, 1U) &&
              !has(receiver, ComponentEditType::Collider),
          "paste is refused during play");
  editor_session().playState = PlayState::Stopped;
  t.check(execute_component_paste_as_new(&receiver, 1U) &&
              (collider_extent(receiver) == 6.0F),
          "after Stop the copied value pastes");
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
  component_clipboard_clear();

  check_reset(t, *world);
  check_copy_paste(t, *world);
  check_documents_and_play(t, *world);

  component_clipboard_clear();
  editor_set_world(nullptr);
  engine::core::shutdown_logging();
  return t.finish("editor_component_clipboard");
}
