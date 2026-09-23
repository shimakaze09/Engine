// Verifies editor session world-transition safety (audit C-04): rebinding
// the editor to a different world must reset play state, selection, and
// the Play snapshot, and a snapshot captured from one world must never be
// restored into another even when session state is forced onto the bug's
// historical path. Also the gesture and allocation contracts of #567:
// history moves record an open gesture first, a gizmo gesture binds its
// own target, and a command the history cannot record never mutates the
// world silently.

#include "editor_commands.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "engine/editor/editor.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

/// Names an entity so cross-world contamination is observable; returns
/// the created entity (kInvalidEntity on failure).
engine::runtime::Entity add_named_entity(engine::runtime::World &world,
                                         const char *name) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return engine::runtime::kInvalidEntity;
  }
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  if (!world.add_name_component(entity, nameComponent)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

/// EXPECTATION: switching the bound world mid-play resets play state,
/// selection, step requests, and discards the Play snapshot.
int check_world_switch_resets_session() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> worldA(new (std::nothrow) World());
  std::unique_ptr<World> worldB(new (std::nothrow) World());
  if ((worldA == nullptr) || (worldB == nullptr)) {
    return 1;
  }

  editor_set_world(worldA.get());
  const Entity onlyInA = add_named_entity(*worldA, "OnlyInA");
  if (onlyInA == kInvalidEntity) {
    editor_set_world(nullptr);
    return 2;
  }
  select_entity(onlyInA, false);
  if (!capture_play_snapshot()) {
    editor_set_world(nullptr);
    return 3;
  }
  editor_session().playState = PlayState::Playing;
  editor_session().stepRequested = true;

  editor_set_world(worldB.get());
  const EditorSession &session = editor_session();
  if ((session.playState != PlayState::Stopped) || session.hasPlaySnapshot ||
      (session.playSnapshotWorld != nullptr) ||
      (session.selectedEntity != kInvalidEntity) ||
      (session.selectedEntityCount != 0U) || session.stepRequested ||
      session.worldRestoreFailed) {
    editor_set_world(nullptr);
    return 4;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: even when the session is forced onto the historical bug
/// path (world swapped without editor_set_world), Stop must refuse to
/// restore world A's snapshot into world B and must fail loudly.
int check_snapshot_never_restores_into_other_world() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> worldA(new (std::nothrow) World());
  std::unique_ptr<World> worldB(new (std::nothrow) World());
  if ((worldA == nullptr) || (worldB == nullptr)) {
    return 10;
  }
  if (add_named_entity(*worldB, "OnlyInB") == kInvalidEntity) {
    return 11;
  }

  editor_set_world(worldA.get());
  if (add_named_entity(*worldA, "OnlyInA") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 12;
  }
  if (!capture_play_snapshot()) {
    editor_set_world(nullptr);
    return 13;
  }
  editor_session().playState = PlayState::Playing;

  editor_session().world = worldB.get();
  stop_play_mode();
  // No pipeline frame runs here, so finish the Stop as the pipeline
  // would after the end hooks.
  finish_play_stop();

  if (worldB->find_entity_by_name("OnlyInB") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 14;
  }
  if (worldB->find_entity_by_name("OnlyInA") != kInvalidEntity) {
    editor_set_world(nullptr);
    return 15;
  }
  if (!editor_session().worldRestoreFailed ||
      editor_session().hasPlaySnapshot) {
    editor_set_world(nullptr);
    return 16;
  }
  if (worldA->find_entity_by_name("OnlyInA") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 17;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: a malformed same-world snapshot must not destroy the live
/// world on Stop — scene loading is transactional, so the play world and
/// the snapshot survive, and the failure is surfaced loudly.
int check_malformed_snapshot_preserves_world() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 20;
  }

  editor_set_world(world.get());
  const Entity survivor = add_named_entity(*world, "Survivor");
  if (survivor == kInvalidEntity) {
    editor_set_world(nullptr);
    return 21;
  }
  if (!capture_play_snapshot()) {
    editor_set_world(nullptr);
    return 22;
  }
  editor_session().playState = PlayState::Playing;
  select_entity(survivor, false);

  std::memcpy(editor_session().playSnapshotBuffer.get(), "garbage!", 8U);

  stop_play_mode();
  finish_play_stop();

  if (world->find_entity_by_name("Survivor") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 23;
  }
  if (!editor_session().worldRestoreFailed ||
      !editor_session().hasPlaySnapshot ||
      (editor_session().playState != PlayState::Stopped)) {
    editor_set_world(nullptr);
    return 24;
  }
  if ((editor_session().selectedEntity != kInvalidEntity) ||
      (editor_session().selectedEntityCount != 0U)) {
    editor_set_world(nullptr);
    return 25;
  }

  // #525: the advertised recovery must work. With the latch set, Save
  // states why it refused instead of silently returning false, and New
  // (the same gate as Open) replaces the preserved world and clears the
  // latch. On base every document operation was gated on the latch, so
  // the editor could only be quit.
  if (perform_scene_save() || (scene_document_last_error()[0] == '\0')) {
    editor_set_world(nullptr);
    return 26;
  }
  if (!perform_scene_new() || editor_session().worldRestoreFailed) {
    editor_set_world(nullptr);
    return 27;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: a selection must never alias a different entity that
/// reuses the deleted entity's index slot; dead handles are pruned from
/// the multi-selection and the primary selection.
int check_selection_rejects_recycled_slot() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 30;
  }
  editor_set_world(world.get());

  const Entity keeper = add_named_entity(*world, "Keeper");
  const Entity doomed = add_named_entity(*world, "Doomed");
  if ((keeper == kInvalidEntity) || (doomed == kInvalidEntity)) {
    editor_set_world(nullptr);
    return 31;
  }

  select_entity(keeper, false);
  select_entity(doomed, true);
  if (!world->destroy_entity(doomed)) {
    editor_set_world(nullptr);
    return 32;
  }

  const Entity recycled = add_named_entity(*world, "Recycled");
  if ((recycled == kInvalidEntity) || (recycled.index != doomed.index) ||
      (recycled.generation == doomed.generation)) {
    editor_set_world(nullptr);
    return 33;
  }

  if (is_entity_selected(recycled) || is_entity_selected(doomed)) {
    editor_set_world(nullptr);
    return 34;
  }

  prune_entity_selection();
  if ((editor_session().selectedEntityCount != 1U) ||
      !is_entity_selected(keeper) || (selected_entity() != keeper)) {
    editor_set_world(nullptr);
    return 35;
  }

  select_entity(doomed, false);
  if (selected_entity() != kInvalidEntity) {
    editor_set_world(nullptr);
    return 36;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: a scene load replaces the world's contents (resetting
/// entity generations), so a retained selection must be invalidated by
/// the content-epoch check even when index+generation happen to match a
/// freshly loaded entity.
int check_selection_invalidated_by_scene_load() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 40;
  }
  editor_set_world(world.get());

  const Entity picked = add_named_entity(*world, "Picked");
  if (picked == kInvalidEntity) {
    editor_set_world(nullptr);
    return 41;
  }
  select_entity(picked, false);

  char sceneBuffer[16384] = {};
  std::size_t sceneSize = 0U;
  if (!save_scene(*world, sceneBuffer, sizeof(sceneBuffer), &sceneSize)) {
    editor_set_world(nullptr);
    return 42;
  }
  if (!load_scene(*world, sceneBuffer, sceneSize)) {
    editor_set_world(nullptr);
    return 43;
  }

  const Entity reloaded = world->find_entity_by_name("Picked");
  if (reloaded == kInvalidEntity) {
    editor_set_world(nullptr);
    return 44;
  }
  if (is_entity_selected(reloaded) || is_entity_selected(picked) ||
      (selected_entity() != kInvalidEntity) ||
      (editor_session().selectedEntityCount != 0U)) {
    editor_set_world(nullptr);
    return 45;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: undo/redo must be inert while the world is not editable
/// (during play), and work again once stopped.
int check_history_gated_while_playing() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 50;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Gated");
  if (entity == kInvalidEntity) {
    editor_set_world(nullptr);
    return 51;
  }

  auto *command = new (std::nothrow) TransformEditCommand();
  if (command == nullptr) {
    editor_set_world(nullptr);
    return 52;
  }
  command->entity = entity;
  command->persistentId = world->persistent_id(entity);
  command->oldTransform = Transform{};
  command->newTransform.position = engine::math::Vec3(5.0F, 0.0F, 0.0F);
  editor_session().commandHistory.execute(command);

  editor_session().playState = PlayState::Playing;
  editor_history_undo();
  Transform duringPlay{};
  if (!world->get_transform(entity, &duringPlay) ||
      (duringPlay.position.x != 5.0F) ||
      !editor_session().commandHistory.can_undo()) {
    editor_set_world(nullptr);
    return 53;
  }

  editor_session().playState = PlayState::Stopped;
  editor_history_undo();
  Transform afterStop{};
  if (!world->get_transform(entity, &afterStop) ||
      (afterStop.position.x != 0.0F)) {
    editor_set_world(nullptr);
    return 54;
  }

  editor_session().playState = PlayState::Paused;
  editor_history_redo();
  Transform duringPause{};
  if (!world->get_transform(entity, &duringPause) ||
      (duringPause.position.x != 0.0F)) {
    editor_set_world(nullptr);
    return 55;
  }

  editor_session().playState = PlayState::Stopped;
  editor_history_redo();
  Transform afterRedo{};
  if (!world->get_transform(entity, &afterRedo) ||
      (afterRedo.position.x != 5.0F)) {
    editor_set_world(nullptr);
    return 56;
  }

  editor_session().commandHistory.clear();
  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION (#350): the primary selection is always a member of the
/// retained multi-selection. A pick that would grow the set past capacity
/// is refused whole — set and primary both unchanged — so the Inspector's
/// bulk target set and the gizmo target can never diverge; freeing a slot
/// lets the same pick succeed normally.
int check_selection_capacity_keeps_primary_in_set() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 60;
  }
  editor_set_world(world.get());

  constexpr std::size_t kCapacity = EditorSession::kMaxSelectedEntities;
  Entity entities[kCapacity + 1U] = {};
  for (std::size_t i = 0U; i <= kCapacity; ++i) {
    entities[i] = world->create_scene_object();
    if (entities[i] == kInvalidEntity) {
      editor_set_world(nullptr);
      return 61;
    }
  }

  select_entity(entities[0], false);
  for (std::size_t i = 1U; i < kCapacity; ++i) {
    select_entity(entities[i], true);
  }
  const Entity lastInSet = entities[kCapacity - 1U];
  if ((editor_session().selectedEntityCount != kCapacity) ||
      (selected_entity() != lastInSet) || !is_entity_selected(lastInSet)) {
    editor_set_world(nullptr);
    return 62;
  }

  // The over-capacity pick changes nothing: not the set, not the primary.
  const Entity overflow = entities[kCapacity];
  select_entity(overflow, true);
  if ((editor_session().selectedEntityCount != kCapacity) ||
      is_entity_selected(overflow) || (selected_entity() != lastInSet) ||
      !is_entity_selected(selected_entity())) {
    editor_set_world(nullptr);
    return 63;
  }

  // Repeating the refused pick stays stable.
  select_entity(overflow, true);
  if ((editor_session().selectedEntityCount != kCapacity) ||
      is_entity_selected(overflow) || (selected_entity() != lastInSet)) {
    editor_set_world(nullptr);
    return 64;
  }

  // Ctrl-click deselecting the primary retargets it to a set member.
  select_entity(lastInSet, true);
  if ((editor_session().selectedEntityCount != kCapacity - 1U) ||
      is_entity_selected(lastInSet) ||
      (selected_entity() != entities[kCapacity - 2U]) ||
      !is_entity_selected(selected_entity())) {
    editor_set_world(nullptr);
    return 65;
  }

  // With a slot free, the previously refused entity joins normally and
  // becomes the primary.
  select_entity(overflow, true);
  if ((editor_session().selectedEntityCount != kCapacity) ||
      !is_entity_selected(overflow) || (selected_entity() != overflow)) {
    editor_set_world(nullptr);
    return 66;
  }

  // A non-additive pick at capacity replaces the whole selection.
  select_entity(overflow, false);
  if ((editor_session().selectedEntityCount != 1U) ||
      !is_entity_selected(overflow) || (selected_entity() != overflow)) {
    editor_set_world(nullptr);
    return 67;
  }

  clear_entity_selection();
  editor_set_world(nullptr);
  return 0;
}

} // namespace


/// Sets an entity's local position through the World (a direct write,
/// the way a drag reaches the world frame by frame).
bool set_position_x(engine::runtime::World &world,
                    engine::runtime::Entity entity, float x) noexcept {
  engine::runtime::Transform transform{};
  if (!world.get_transform(entity, &transform)) {
    return false;
  }
  transform.position.x = x;
  return world.add_transform(entity, transform);
}

/// Reads an entity's local x position; NaN-free sentinel on failure.
float position_x(engine::runtime::World &world,
                 engine::runtime::Entity entity) noexcept {
  engine::runtime::Transform transform{};
  if (!world.get_transform(entity, &transform)) {
    return -1000.0F;
  }
  return transform.position.x;
}

/// Opens an inspector Transform gesture moving `entity` from `fromX` to
/// `toX`, the way the Inspector's drag widget stages it every frame.
bool stage_inspector_move(engine::runtime::World &world,
                          engine::runtime::Entity entity, float fromX,
                          float toX) noexcept {
  using namespace engine::editor;
  ComponentEditSnapshot before{};
  if (!world.get_transform(entity, &before.transform)) {
    return false;
  }
  before.transform.position.x = fromX;
  ComponentEditSnapshot after = before;
  after.transform.position.x = toX;
  return inspector_stage_component_edit(entity, ComponentEditType::Transform,
                                        before, after);
}

/// Marks the document clean at the current history position, as a save
/// does, so the next check observes only its own mutation.
void mark_document_saved() noexcept {
  using namespace engine::editor;
  editor_session().document.savedHistoryToken =
      editor_session().commandHistory.current_token();
  editor_session().document.unrecordedEdit = false;
}

/// EXPECTATION (#567 row 1): undo, redo, reparent and a gizmo edit
/// record a pending inspector gesture before they run, so the gesture's
/// command lands in order and its own undo restores the state it started
/// from. On base the undo ran under the open gesture and the gesture was
/// committed afterwards with a snapshot from before it.
int check_history_moves_commit_pending_gesture() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 60;
  }
  editor_set_world(world.get());
  const Entity entity = add_named_entity(*world, "Gesture");
  const Entity parent = add_named_entity(*world, "Parent");
  if ((entity == kInvalidEntity) || (parent == kInvalidEntity)) {
    editor_set_world(nullptr);
    return 61;
  }

  // Recorded edit X: 0 -> 5. Then a gesture 5 -> 7 still open.
  Transform start{};
  Transform moved{};
  moved.position.x = 5.0F;
  if (!execute_transform_edit(entity, start, moved) ||
      !stage_inspector_move(*world, entity, 5.0F, 7.0F) ||
      !inspector_has_pending_edit() || (position_x(*world, entity) != 7.0F)) {
    editor_set_world(nullptr);
    return 62;
  }

  // Ctrl+Z mid-gesture: the gesture is committed then undone, X stays.
  editor_history_undo();
  if (inspector_has_pending_edit() || (position_x(*world, entity) != 5.0F) ||
      !editor_session().commandHistory.can_redo()) {
    std::fprintf(stderr,
                 "undo ran under an open gesture: x=%.1f pending=%d\n",
                 position_x(*world, entity),
                 inspector_has_pending_edit() ? 1 : 0);
    editor_set_world(nullptr);
    return 63;
  }
  editor_history_undo();
  if (position_x(*world, entity) != 0.0F) {
    editor_set_world(nullptr);
    return 64;
  }
  editor_history_redo();
  editor_history_redo();
  if (position_x(*world, entity) != 7.0F) {
    editor_set_world(nullptr);
    return 65;
  }

  // A reparent mid-gesture records the gesture first: two undos restore
  // both the parent and the pre-gesture position, in order.
  if (!stage_inspector_move(*world, entity, 7.0F, 9.0F) ||
      !execute_reparent(entity, parent)) {
    editor_set_world(nullptr);
    return 66;
  }
  if (inspector_has_pending_edit()) {
    std::fprintf(stderr, "reparent ran under an open gesture\n");
    editor_set_world(nullptr);
    return 67;
  }
  editor_history_undo();
  Transform afterFirstUndo{};
  if (!world->get_transform(entity, &afterFirstUndo) ||
      (afterFirstUndo.parentId != kInvalidPersistentId) ||
      (afterFirstUndo.position.x != 9.0F)) {
    editor_set_world(nullptr);
    return 68;
  }
  editor_history_undo();
  if (position_x(*world, entity) != 7.0F) {
    editor_set_world(nullptr);
    return 69;
  }

  // A gizmo drag ending mid-gesture records the gesture first too.
  Transform current{};
  if (!stage_inspector_move(*world, entity, 7.0F, 8.0F) ||
      !world->get_transform(entity, &current)) {
    editor_set_world(nullptr);
    return 70;
  }
  gizmo_track_gesture(entity, true, current);
  if (!set_position_x(*world, entity, 3.0F)) {
    editor_set_world(nullptr);
    return 71;
  }
  gizmo_track_gesture(entity, false, current);
  if (inspector_has_pending_edit()) {
    std::fprintf(stderr, "gizmo edit recorded under an open gesture\n");
    editor_set_world(nullptr);
    return 72;
  }
  editor_history_undo();
  if (position_x(*world, entity) != 8.0F) {
    editor_set_world(nullptr);
    return 73;
  }
  editor_history_undo();
  if (position_x(*world, entity) != 7.0F) {
    editor_set_world(nullptr);
    return 74;
  }

  editor_session().commandHistory.clear();
  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION (#567 row 2): a gizmo gesture records against the entity
/// it opened on, with that entity's own start transform; a scene switch
/// or Stop mid-gesture drops it, and the next entity under the gizmo
/// never inherits a stale start. On base the end-of-drag paired the
/// stale start with whatever was selected at release.
int check_gizmo_gesture_binds_its_target() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 80;
  }
  editor_set_world(world.get());
  Entity a = add_named_entity(*world, "A");
  if ((a == kInvalidEntity) || !set_position_x(*world, a, 1.0F)) {
    editor_set_world(nullptr);
    return 81;
  }
  Transform ta{};
  world->get_transform(a, &ta);

  // Scene switch (New) mid-gesture: the gesture dies with the contents.
  gizmo_track_gesture(a, true, ta);
  if (!gizmo_has_gesture() || !perform_scene_new() || gizmo_has_gesture()) {
    std::fprintf(stderr, "gizmo gesture survived the scene switch\n");
    editor_set_world(nullptr);
    return 82;
  }
  Entity b = add_named_entity(*world, "B");
  if ((b == kInvalidEntity) || !set_position_x(*world, b, 2.0F)) {
    editor_set_world(nullptr);
    return 83;
  }
  Transform tb{};
  world->get_transform(b, &tb);
  gizmo_track_gesture(b, false, tb);
  if (editor_session().commandHistory.can_undo() ||
      (position_x(*world, b) != 2.0F)) {
    std::fprintf(stderr, "a stale gesture recorded against the next "
                         "entity after a scene switch\n");
    editor_set_world(nullptr);
    return 84;
  }

  // Play then Stop mid-gesture: Play drops the gesture (the viewport is
  // not editable while playing) and the Stop restore replaces the
  // contents; neither may leave a gesture to close against the restored
  // entities.
  a = add_named_entity(*world, "A2");
  if ((a == kInvalidEntity) || !set_position_x(*world, a, 1.0F)) {
    editor_set_world(nullptr);
    return 85;
  }
  world->get_transform(a, &ta);
  editor_session().commandHistory.clear();
  gizmo_track_gesture(a, true, ta);
  start_play_mode();
  stop_play_mode();
  finish_play_stop();
  if (gizmo_has_gesture() ||
      (editor_session().playState != PlayState::Stopped)) {
    std::fprintf(stderr, "gizmo gesture survived Stop\n");
    editor_set_world(nullptr);
    return 86;
  }
  b = world->find_entity_by_name("B");
  world->get_transform(b, &tb);
  gizmo_track_gesture(b, false, tb);
  if (editor_session().commandHistory.can_undo() ||
      (position_x(*world, b) != 2.0F)) {
    std::fprintf(stderr, "a stale gesture recorded after Stop\n");
    editor_set_world(nullptr);
    return 87;
  }

  // Target change mid-gesture: each entity's edit is its own command
  // with its own start transform.
  a = world->find_entity_by_name("A2");
  world->get_transform(a, &ta);
  gizmo_track_gesture(a, true, ta);
  if (!set_position_x(*world, a, 1.5F)) {
    editor_set_world(nullptr);
    return 88;
  }
  gizmo_track_gesture(b, true, tb);
  if (!set_position_x(*world, b, 2.5F)) {
    editor_set_world(nullptr);
    return 89;
  }
  gizmo_track_gesture(b, false, tb);
  editor_history_undo();
  if ((position_x(*world, b) != 2.0F) || (position_x(*world, a) != 1.5F)) {
    std::fprintf(stderr, "undo of the second gesture moved the wrong "
                         "entity: a=%.1f b=%.1f\n",
                 position_x(*world, a), position_x(*world, b));
    editor_set_world(nullptr);
    return 90;
  }
  editor_history_undo();
  if ((position_x(*world, a) != 1.0F) || (position_x(*world, b) != 2.0F)) {
    editor_set_world(nullptr);
    return 91;
  }

  editor_session().commandHistory.clear();
  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION (#567 row 3): when a command cannot be allocated, no
/// mutation reaches the world outside the history, except a gesture that
/// already did, which then keeps the document dirty until a save or a
/// content replacement. On base the fallbacks mutated silently and the
/// token-based dirty check read clean.
int check_allocation_failure_is_never_silent() {
  using namespace engine::editor;
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 100;
  }
  editor_set_world(world.get());
  const Entity entity = add_named_entity(*world, "Alloc");
  if (entity == kInvalidEntity) {
    editor_set_world(nullptr);
    return 101;
  }
  mark_document_saved();

  // A gesture that already reached the world: latched dirty.
  if (!stage_inspector_move(*world, entity, 0.0F, 7.0F)) {
    editor_set_world(nullptr);
    return 102;
  }
  editor_commands_inject_allocation_failures(1U);
  inspector_commit_pending_edit();
  if ((position_x(*world, entity) != 7.0F) || !scene_document_is_dirty()) {
    std::fprintf(stderr, "unrecorded gesture left the document clean\n");
    editor_set_world(nullptr);
    return 103;
  }
  mark_document_saved();
  if (scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 104;
  }

  // Component add: refused, nothing applied.
  editor_commands_inject_allocation_failures(1U);
  execute_component_add(
      entity, ComponentEditType::PointLight,
      default_component_snapshot(entity, ComponentEditType::PointLight));
  if (world->has_point_light_component(entity) &&
      !scene_document_is_dirty()) {
    std::fprintf(stderr, "component add applied silently\n");
    editor_set_world(nullptr);
    return 105;
  }
  if (world->has_point_light_component(entity)) {
    editor_set_world(nullptr);
    return 106;
  }

  // Component remove: refused, component stays.
  execute_component_add(
      entity, ComponentEditType::PointLight,
      default_component_snapshot(entity, ComponentEditType::PointLight));
  mark_document_saved();
  editor_commands_inject_allocation_failures(1U);
  execute_component_remove(entity, ComponentEditType::PointLight);
  if (!world->has_point_light_component(entity) &&
      !scene_document_is_dirty()) {
    std::fprintf(stderr, "component remove applied silently\n");
    editor_set_world(nullptr);
    return 107;
  }
  if (!world->has_point_light_component(entity)) {
    editor_set_world(nullptr);
    return 108;
  }

  // Entity create: refused, nothing created.
  const std::size_t aliveBefore = world->alive_entity_count();
  editor_commands_inject_allocation_failures(1U);
  const Entity created = execute_entity_create();
  if ((created != kInvalidEntity) ||
      (world->alive_entity_count() != aliveBefore)) {
    std::fprintf(stderr, "entity create applied silently\n");
    editor_set_world(nullptr);
    return 109;
  }

  // Entity delete: refused, entity stays.
  editor_commands_inject_allocation_failures(1U);
  if (execute_entity_delete(entity) || !world->is_alive(entity)) {
    std::fprintf(stderr, "entity delete applied silently\n");
    editor_set_world(nullptr);
    return 110;
  }

  editor_commands_inject_allocation_failures(0U);
  editor_session().commandHistory.clear();
  editor_set_world(nullptr);
  return 0;
}

/// Runs this executable or test program.
int main() {
  int result = check_world_switch_resets_session();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_snapshot_never_restores_into_other_world();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_malformed_snapshot_preserves_world();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_selection_rejects_recycled_slot();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_selection_invalidated_by_scene_load();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_history_gated_while_playing();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_selection_capacity_keeps_primary_in_set();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_history_moves_commit_pending_gesture();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_gizmo_gesture_binds_its_target();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  result = check_allocation_failure_is_never_silent();
  if (result != 0) {
    std::fprintf(stderr, "editor_session_test failed: %d\n", result);
    return result;
  }

  std::printf("editor_session_test: all tests passed\n");
  return 0;
}
