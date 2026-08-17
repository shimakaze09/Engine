// Verifies play-mode live-edit support (issue #159): live edits require the
// explicit opt-in toggle, never reach command history, Stop always restores
// the authored snapshot regardless of untracked live edits, Revert restores
// the play-session baseline, and a queued "Apply to authored value" replays
// as one ordinary undoable command once Stop has restored the authored
// world -- proving the apply path integrates with the normal undo/dirty
// contract instead of bypassing it.

#include "editor_commands.h"
#include "editor_live_edit.h"
#include "editor_session.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <memory>
#include <new>

namespace {

using namespace engine::editor;
using engine::runtime::Entity;
using engine::runtime::RigidBody;
using engine::runtime::World;

/// Binds a fresh world to the editor session; restores prior state (and
/// drops any live-edit tracking) on destruction so tests never leak into
/// each other via the process-wide session/live-edit tables.
struct SessionWorldScope final {
  World *previousWorld = nullptr;
  PlayState previousPlayState = PlayState::Stopped;
  bool previousLiveEdit = false;

  explicit SessionWorldScope(World *world) noexcept {
    EditorSession &session = editor_session();
    previousWorld = session.world;
    previousPlayState = session.playState;
    previousLiveEdit = session.liveEditEnabled;
    session.world = world;
    session.playState = PlayState::Stopped;
    session.worldRestoreFailed = false;
    session.liveEditEnabled = false;
  }

  ~SessionWorldScope() noexcept {
    EditorSession &session = editor_session();
    session.playState = PlayState::Stopped;
    reset_live_edit_state();
    session.commandHistory.clear();
    session.world = previousWorld;
    session.playState = previousPlayState;
    session.liveEditEnabled = previousLiveEdit;
  }
};

Entity make_rigid_body_entity(World &world, float inverseMass) noexcept {
  const Entity entity = world.create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return engine::runtime::kInvalidEntity;
  }
  RigidBody body{};
  body.inverseMass = inverseMass;
  if (!world.add_rigid_body(entity, body)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

/// EXPECTATION: with the toggle off, live-edit functions all refuse (false)
/// and leave the running world untouched -- the default Play/Pause
/// behavior stays pure read-only inspection.
int check_live_edit_requires_opt_in() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  SessionWorldScope scope(world.get());
  const Entity entity = make_rigid_body_entity(*world, 1.0F);
  if (entity == engine::runtime::kInvalidEntity) {
    return 2;
  }

  start_play_mode();
  if (editor_session().playState != PlayState::Playing) {
    return 3;
  }
  if (live_edit_available()) {
    return 4; // toggle is off by construction
  }

  ComponentEditSnapshot after{};
  after.rigidBody.inverseMass = 9.0F;
  if (apply_live_component_edit(entity, ComponentEditType::RigidBody, after)) {
    return 5;
  }
  RigidBody unchanged{};
  if (!world->get_rigid_body(entity, &unchanged) ||
      (unchanged.inverseMass != 1.0F)) {
    return 6;
  }

  stop_play_mode();
  return 0;
}

/// EXPECTATION: with the toggle on, a live edit applies straight to the
/// running world, never opens or commits a command-history entry, and
/// Stop discards it (restores the authored value) since it was never
/// applied to authored state.
int check_live_edit_bypasses_history_and_is_discarded_on_stop() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 10;
  }
  SessionWorldScope scope(world.get());
  const Entity entity = make_rigid_body_entity(*world, 1.0F);
  if (entity == engine::runtime::kInvalidEntity) {
    return 11;
  }

  editor_session().liveEditEnabled = true;
  start_play_mode();
  if (!live_edit_available()) {
    return 12;
  }

  ComponentEditSnapshot after{};
  after.rigidBody.inverseMass = 9.0F;
  if (!apply_live_component_edit(entity, ComponentEditType::RigidBody, after)) {
    return 13;
  }
  RigidBody live{};
  if (!world->get_rigid_body(entity, &live) || (live.inverseMass != 9.0F)) {
    return 14;
  }
  if (editor_session().commandHistory.can_undo()) {
    return 15; // must never reach history
  }
  if (!has_live_component_edit(entity, ComponentEditType::RigidBody)) {
    return 16;
  }

  stop_play_mode();
  if (editor_session().worldRestoreFailed) {
    return 17;
  }
  RigidBody restored{};
  if (!world->get_rigid_body(entity, &restored) ||
      (restored.inverseMass != 1.0F)) {
    return 18; // authored value must win; the live tweak was never applied
  }
  if (editor_session().commandHistory.can_undo()) {
    return 19; // Stop's restore is not itself an undoable edit
  }

  return 0;
}

/// EXPECTATION: Revert restores the play-session baseline (the value
/// before the first live touch) and clears the "edited live" badge state.
int check_revert_restores_baseline() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 20;
  }
  SessionWorldScope scope(world.get());
  const Entity entity = make_rigid_body_entity(*world, 2.0F);
  if (entity == engine::runtime::kInvalidEntity) {
    return 21;
  }

  editor_session().liveEditEnabled = true;
  start_play_mode();

  ComponentEditSnapshot step1{};
  step1.rigidBody.inverseMass = 5.0F;
  if (!apply_live_component_edit(entity, ComponentEditType::RigidBody, step1)) {
    return 22;
  }
  ComponentEditSnapshot step2{};
  step2.rigidBody.inverseMass = 7.0F;
  if (!apply_live_component_edit(entity, ComponentEditType::RigidBody, step2)) {
    return 23;
  }

  if (!revert_live_component_edit(entity, ComponentEditType::RigidBody)) {
    return 24;
  }
  RigidBody reverted{};
  if (!world->get_rigid_body(entity, &reverted) ||
      (reverted.inverseMass != 2.0F)) {
    return 25; // baseline captured at the *first* touch (2.0), not step1
  }
  if (has_live_component_edit(entity, ComponentEditType::RigidBody)) {
    return 26;
  }

  stop_play_mode();
  return 0;
}

/// EXPECTATION: "Apply to authored value" queues the running value; Stop's
/// restore discards the live world first, then replays the queued value as
/// ONE ordinary undoable ComponentEditCommand against the restored
/// authored world, and undo reaches the original authored value.
int check_apply_to_authored_replays_as_undoable_command() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 30;
  }
  SessionWorldScope scope(world.get());
  const Entity entity = make_rigid_body_entity(*world, 1.0F);
  if (entity == engine::runtime::kInvalidEntity) {
    return 31;
  }

  editor_session().liveEditEnabled = true;
  start_play_mode();

  ComponentEditSnapshot after{};
  after.rigidBody.inverseMass = 4.0F;
  if (!apply_live_component_edit(entity, ComponentEditType::RigidBody, after)) {
    return 32;
  }
  if (!queue_apply_to_authored(entity, ComponentEditType::RigidBody)) {
    return 33;
  }
  if (!has_pending_apply_to_authored(entity, ComponentEditType::RigidBody)) {
    return 34;
  }
  if (pending_apply_to_authored_count() != 1U) {
    return 35;
  }

  stop_play_mode();
  if (editor_session().worldRestoreFailed) {
    return 36;
  }
  RigidBody applied{};
  if (!world->get_rigid_body(entity, &applied) ||
      (applied.inverseMass != 4.0F)) {
    return 37; // the queued value must have replaced the restored 1.0
  }
  if (!editor_session().commandHistory.can_undo()) {
    return 38; // must land in history as an ordinary edit
  }
  if (pending_apply_to_authored_count() != 0U) {
    return 39; // the queue must be drained after replay
  }

  editor_session().commandHistory.undo();
  RigidBody undone{};
  if (!world->get_rigid_body(entity, &undone) || (undone.inverseMass != 1.0F)) {
    return 40; // undo must reach the pre-play authored value
  }

  editor_session().commandHistory.redo();
  RigidBody redone{};
  if (!world->get_rigid_body(entity, &redone) || (redone.inverseMass != 4.0F)) {
    return 41;
  }

  return 0;
}

/// EXPECTATION: canceling a queued apply drops it without ever reaching
/// Stop's replay, so the authored value is untouched.
int check_cancel_apply_to_authored() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 50;
  }
  SessionWorldScope scope(world.get());
  const Entity entity = make_rigid_body_entity(*world, 1.0F);
  if (entity == engine::runtime::kInvalidEntity) {
    return 51;
  }

  editor_session().liveEditEnabled = true;
  start_play_mode();

  ComponentEditSnapshot after{};
  after.rigidBody.inverseMass = 6.0F;
  if (!apply_live_component_edit(entity, ComponentEditType::RigidBody, after) ||
      !queue_apply_to_authored(entity, ComponentEditType::RigidBody)) {
    return 52;
  }
  cancel_apply_to_authored(entity, ComponentEditType::RigidBody);
  if (has_pending_apply_to_authored(entity, ComponentEditType::RigidBody)) {
    return 53;
  }

  stop_play_mode();
  RigidBody restored{};
  if (!world->get_rigid_body(entity, &restored) ||
      (restored.inverseMass != 1.0F)) {
    return 54;
  }
  if (editor_session().commandHistory.can_undo()) {
    return 55;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
/// EXPECTATION (audit #224): the live-edit baseline budget is keyed by
/// (entity, component type) pairs — one entity consumes a slot per touched
/// type — and a first-touch edit past capacity is refused BEFORE mutating
/// the world, keeping the advertised Revert contract intact; reverting an
/// edit frees its slot for a new pair.
int check_baseline_budget_blocks_edit_before_mutation() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 90;
  }
  SessionWorldScope scope(world.get());

  // One entity, two component types: two budget slots, proving the key is
  // the (entity, type) pair rather than the entity.
  const Entity dual = make_rigid_body_entity(*world, 1.0F);
  if (dual == engine::runtime::kInvalidEntity) {
    return 91;
  }
  const std::size_t capacity = live_edit_baseline_capacity();
  if (capacity < 3U) {
    return 92; // the fill loop below assumes a real budget
  }

  start_play_mode();
  editor_session().liveEditEnabled = true;
  if (!live_edit_available()) {
    return 93;
  }

  ComponentEditSnapshot bodyEdit{};
  bodyEdit.rigidBody.inverseMass = 9.0F;
  if (!apply_live_component_edit(dual, ComponentEditType::RigidBody,
                                 bodyEdit)) {
    return 94;
  }
  ComponentEditSnapshot transformEdit{};
  transformEdit.transform.position.x = 5.0F;
  if (!apply_live_component_edit(dual, ComponentEditType::Transform,
                                 transformEdit)) {
    return 95;
  }
  if (live_edit_baseline_count() != 2U) {
    return 96; // one entity, two types -> two slots
  }

  // Fill the remaining budget with sequentially edited entities (never
  // more than one selected at a time in a real session).
  for (std::size_t i = live_edit_baseline_count(); i < capacity; ++i) {
    const Entity extra = make_rigid_body_entity(
        *world, static_cast<float>(i) + 1.0F);
    if (extra == engine::runtime::kInvalidEntity) {
      return 97;
    }
    if (!apply_live_component_edit(extra, ComponentEditType::RigidBody,
                                   bodyEdit)) {
      return 98;
    }
  }
  if (live_edit_baseline_count() != capacity) {
    return 99;
  }

  // Capacity + 1: refused before mutation, no badge, value untouched.
  const Entity overflow = make_rigid_body_entity(*world, 77.0F);
  if (overflow == engine::runtime::kInvalidEntity) {
    return 100;
  }
  if (apply_live_component_edit(overflow, ComponentEditType::RigidBody,
                                bodyEdit)) {
    return 101;
  }
  RigidBody untouched{};
  if (!world->get_rigid_body(overflow, &untouched) ||
      (untouched.inverseMass != 77.0F)) {
    return 102; // the refused edit must not have mutated the world
  }
  if (has_live_component_edit(overflow, ComponentEditType::RigidBody)) {
    return 103;
  }
  // An already-tracked pair keeps editing at full capacity (no new slot).
  bodyEdit.rigidBody.inverseMass = 12.0F;
  if (!apply_live_component_edit(dual, ComponentEditType::RigidBody,
                                 bodyEdit)) {
    return 104;
  }

  // Revert frees a slot; the blocked pair then succeeds.
  if (!revert_live_component_edit(dual, ComponentEditType::RigidBody)) {
    return 105;
  }
  if (live_edit_baseline_count() != capacity - 1U) {
    return 106;
  }
  if (!apply_live_component_edit(overflow, ComponentEditType::RigidBody,
                                 bodyEdit)) {
    return 107;
  }

  stop_play_mode();
  return 0;
}

/// EXPECTATION (audit #224): the apply-to-authored queue reports failure at
/// capacity + 1 and keeps its count, while re-queueing an already-queued
/// pair replaces in place and still succeeds at full capacity.
int check_pending_apply_budget() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 120;
  }
  SessionWorldScope scope(world.get());

  const std::size_t capacity = pending_apply_to_authored_capacity();
  if (capacity < 2U) {
    return 121;
  }
  std::unique_ptr<Entity[]> entities(new (std::nothrow) Entity[capacity + 1U]);
  if (entities == nullptr) {
    return 122;
  }
  for (std::size_t i = 0U; i < capacity + 1U; ++i) {
    entities[i] = make_rigid_body_entity(*world, static_cast<float>(i) + 1.0F);
    if (entities[i] == engine::runtime::kInvalidEntity) {
      return 123;
    }
  }

  start_play_mode();
  editor_session().liveEditEnabled = true;
  if (!live_edit_available()) {
    return 124;
  }

  for (std::size_t i = 0U; i < capacity; ++i) {
    if (!queue_apply_to_authored(entities[i], ComponentEditType::RigidBody)) {
      return 125;
    }
  }
  if (pending_apply_to_authored_count() != capacity) {
    return 126;
  }

  // Capacity + 1: fails and the count is unchanged.
  if (queue_apply_to_authored(entities[capacity],
                              ComponentEditType::RigidBody)) {
    return 127;
  }
  if (pending_apply_to_authored_count() != capacity) {
    return 128;
  }
  if (has_pending_apply_to_authored(entities[capacity],
                                    ComponentEditType::RigidBody)) {
    return 129;
  }

  // Replacement of an existing pair never allocates, so it succeeds at
  // full capacity and the count stays flat.
  if (!queue_apply_to_authored(entities[0], ComponentEditType::RigidBody)) {
    return 130;
  }
  if (pending_apply_to_authored_count() != capacity) {
    return 131;
  }

  // Cancel frees a slot; the previously blocked pair then queues.
  cancel_apply_to_authored(entities[0], ComponentEditType::RigidBody);
  if (!queue_apply_to_authored(entities[capacity],
                               ComponentEditType::RigidBody)) {
    return 132;
  }
  if (pending_apply_to_authored_count() != capacity) {
    return 133;
  }

  stop_play_mode();
  return 0;
}

int main() {
  struct NamedCheck {
    const char *name;
    int (*fn)();
  };
  const NamedCheck checks[] = {
      {"check_live_edit_requires_opt_in", &check_live_edit_requires_opt_in},
      {"check_live_edit_bypasses_history_and_is_discarded_on_stop",
       &check_live_edit_bypasses_history_and_is_discarded_on_stop},
      {"check_revert_restores_baseline", &check_revert_restores_baseline},
      {"check_apply_to_authored_replays_as_undoable_command",
       &check_apply_to_authored_replays_as_undoable_command},
      {"check_cancel_apply_to_authored", &check_cancel_apply_to_authored},
      {"check_baseline_budget_blocks_edit_before_mutation",
       &check_baseline_budget_blocks_edit_before_mutation},
      {"check_pending_apply_budget", &check_pending_apply_budget},
  };

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_live_edit_test: %s failed: %d\n",
                   check.name, result);
      return result;
    }
  }

  std::printf("editor_live_edit_test: all tests passed\n");
  return 0;
}
