// Verifies lifecycle test behavior for the Engine test suite.

#include "engine/runtime/world.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

// Integration test for P1-M2-A1d:
// Create entity → verify BeginPlay fires once → tick 3 frames verifying Tick
// fires each → destroy → verify EndPlay fires once.

namespace {

using namespace engine::runtime;

bool test_lifecycle_begin_play() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world) {
    std::fprintf(stderr, "FAIL: could not allocate World\n");
    return false;
  }

  const Entity e1 = world->create_entity();
  if (e1.index == 0U) {
    std::fprintf(stderr, "FAIL: create_entity returned invalid\n");
    return false;
  }

  const Transform t{};
  if (!world->add_transform(e1, t)) {
    std::fprintf(stderr, "FAIL: add_transform\n");
    return false;
  }

  // Entity should need begin_play.
  std::size_t needsBeginPlayCount = 0U;
  world->for_each_needs_begin_play(
      [&needsBeginPlayCount](Entity) noexcept { ++needsBeginPlayCount; });
  if (needsBeginPlayCount != 1U) {
    std::fprintf(stderr,
                 "FAIL: expected 1 entity needing begin_play, got %zu\n",
                 needsBeginPlayCount);
    return false;
  }

  // Transition through BeginPlay phase.
  world->begin_begin_play_phase();
  if (world->current_phase() != WorldPhase::BeginPlay) {
    std::fprintf(stderr, "FAIL: expected BeginPlay phase\n");
    return false;
  }

  world->for_each_needs_begin_play([&world](Entity entity) noexcept {
    world->mark_begin_play_done(entity);
  });

  world->end_begin_play_phase();
  if (world->current_phase() != WorldPhase::Input) {
    std::fprintf(stderr, "FAIL: expected Input phase after end_begin_play\n");
    return false;
  }

  // Second call should find no entities needing begin_play.
  std::size_t secondCount = 0U;
  world->for_each_needs_begin_play(
      [&secondCount](Entity) noexcept { ++secondCount; });
  if (secondCount != 0U) {
    std::fprintf(stderr,
                 "FAIL: expected 0 entities needing begin_play after mark, "
                 "got %zu\n",
                 secondCount);
    return false;
  }

  return true;
}

bool test_lifecycle_tick() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;

  const Entity e1 = world->create_entity();
  const Transform t{};
  static_cast<void>(world->add_transform(e1, t));

  // Mark begin_play done.
  world->begin_begin_play_phase();
  world->mark_begin_play_done(e1);
  world->end_begin_play_phase();

  // Simulate 3 update steps.
  constexpr float kDt = 1.0F / 60.0F;
  for (int step = 0; step < 3; ++step) {
    world->begin_update_phase();
    static_cast<void>(world->update_transforms(kDt));
    world->commit_update_phase();
    world->begin_transform_phase();
    world->begin_render_prep_phase();
    world->begin_render_phase();
    world->end_frame_phase();
  }

  // Entity should still be alive after 3 steps.
  if (!world->is_alive(e1)) {
    std::fprintf(stderr, "FAIL: entity should be alive after 3 ticks\n");
    return false;
  }

  return true;
}

bool test_lifecycle_end_play() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;

  const Entity e1 = world->create_entity();
  const Transform t{};
  static_cast<void>(world->add_transform(e1, t));

  world->begin_begin_play_phase();
  world->mark_begin_play_done(e1);
  world->end_begin_play_phase();

  // Enter Simulation so destroy is deferred.
  world->begin_update_phase();
  if (!world->destroy_entity(e1)) {
    std::fprintf(stderr, "FAIL: destroy_entity returned false\n");
    return false;
  }

  // Entity should still be alive (deferred).
  if (!world->is_alive(e1)) {
    std::fprintf(stderr, "FAIL: entity should be alive before EndPlay flush\n");
    return false;
  }

  // Finish frame without begin_update_step so commit_update_phase works
  // even without a swap pending. Go directly through transform/render.
  world->begin_transform_phase();
  world->begin_render_prep_phase();
  world->begin_render_phase();

  // EndPlay phase: iterate pending destroys.
  std::size_t endPlayCount = 0U;
  world->begin_end_play_phase();
  if (world->current_phase() != WorldPhase::EndPlay) {
    std::fprintf(stderr, "FAIL: expected EndPlay phase\n");
    return false;
  }

  world->for_each_pending_destroy(
      [&endPlayCount](Entity) noexcept { ++endPlayCount; });

  if (endPlayCount != 1U) {
    std::fprintf(stderr,
                 "FAIL: expected 1 entity in pending destroy, got %zu\n",
                 endPlayCount);
    return false;
  }

  world->end_end_play_phase();

  // Entity should be dead after EndPlay flush.
  if (world->is_alive(e1)) {
    std::fprintf(stderr, "FAIL: entity should be dead after EndPlay flush\n");
    return false;
  }

  return true;
}

bool test_full_lifecycle_sequence() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;

  // Create entity with components.
  const Entity e1 = world->create_entity();
  const Transform t{};
  static_cast<void>(world->add_transform(e1, t));

  // BeginPlay fires once.
  world->begin_begin_play_phase();
  std::size_t beginPlayFired = 0U;
  world->for_each_needs_begin_play(
      [&world, &beginPlayFired](Entity entity) noexcept {
        world->mark_begin_play_done(entity);
        ++beginPlayFired;
      });
  world->end_begin_play_phase();

  if (beginPlayFired != 1U) {
    std::fprintf(stderr, "FAIL: BeginPlay should fire exactly once, got %zu\n",
                 beginPlayFired);
    return false;
  }

  // Tick 3 frames.
  constexpr float kDt = 1.0F / 60.0F;
  std::size_t tickCount = 0U;
  for (int step = 0; step < 3; ++step) {
    // Check no BeginPlay needed on subsequent frames.
    world->begin_begin_play_phase();
    std::size_t extraBeginPlay = 0U;
    world->for_each_needs_begin_play(
        [&extraBeginPlay](Entity) noexcept { ++extraBeginPlay; });
    world->end_begin_play_phase();

    if (extraBeginPlay != 0U) {
      std::fprintf(stderr, "FAIL: BeginPlay should not fire again on tick %d\n",
                   step);
      return false;
    }

    world->begin_update_phase();
    static_cast<void>(world->update_transforms(kDt));
    world->commit_update_phase();

    ++tickCount;

    world->begin_transform_phase();
    world->begin_render_prep_phase();
    world->begin_render_phase();
    world->end_frame_phase();
  }

  if (tickCount != 3U) {
    std::fprintf(stderr, "FAIL: expected 3 ticks, got %zu\n", tickCount);
    return false;
  }

  // Destroy during Simulation so it is deferred → EndPlay can fire.
  world->begin_update_phase();
  static_cast<void>(world->destroy_entity(e1));
  world->begin_transform_phase();
  world->begin_render_prep_phase();
  world->begin_render_phase();

  // EndPlay fires once.
  world->begin_end_play_phase();
  std::size_t endPlayFired = 0U;
  world->for_each_pending_destroy(
      [&endPlayFired](Entity) noexcept { ++endPlayFired; });
  world->end_end_play_phase();

  if (endPlayFired != 1U) {
    std::fprintf(stderr, "FAIL: EndPlay should fire exactly once, got %zu\n",
                 endPlayFired);
    return false;
  }

  if (world->is_alive(e1)) {
    std::fprintf(stderr, "FAIL: entity should be dead after EndPlay\n");
    return false;
  }

  return true;
}

// Regression: the frame pipeline calls end_frame_phase inside the frame
// graph and only reaches the EndPlay phase afterwards, so a deferred destroy
// must survive end_frame_phase for EndPlay dispatch to ever see it.
bool test_deferred_destroy_survives_end_frame() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;

  const Entity e1 = world->create_entity();
  const Transform t{};
  static_cast<void>(world->add_transform(e1, t));

  world->begin_begin_play_phase();
  world->mark_begin_play_done(e1);
  world->end_begin_play_phase();

  world->begin_update_phase();
  if (!world->destroy_entity(e1)) {
    std::fprintf(stderr, "FAIL: destroy_entity returned false\n");
    return false;
  }
  world->begin_transform_phase();
  world->begin_render_prep_phase();
  world->begin_render_phase();
  world->end_frame_phase();

  if (!world->is_alive(e1)) {
    std::fprintf(stderr,
                 "FAIL: end_frame_phase must not flush pending destroys\n");
    return false;
  }
  if (world->pending_destroy_count() != 1U) {
    std::fprintf(stderr, "FAIL: expected 1 pending destroy, got %zu\n",
                 world->pending_destroy_count());
    return false;
  }

  std::size_t endPlayCount = 0U;
  world->begin_end_play_phase();
  world->for_each_pending_destroy(
      [&endPlayCount](Entity) noexcept { ++endPlayCount; });
  world->end_end_play_phase();

  if (endPlayCount != 1U) {
    std::fprintf(stderr, "FAIL: EndPlay should see 1 entity, got %zu\n",
                 endPlayCount);
    return false;
  }
  if (world->is_alive(e1) || (world->pending_destroy_count() != 0U)) {
    std::fprintf(stderr, "FAIL: entity should be dead after EndPlay flush\n");
    return false;
  }

  return true;
}

// Subtree iteration order feeds EndPlay dispatch: descendants first, root
// last, and only alive members.
bool test_subtree_member_iteration() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;

  const Entity parent = world->create_scene_object();
  const Entity child = world->create_scene_object();
  const Entity grandchild = world->create_scene_object();
  const Entity unrelated = world->create_scene_object();
  static_cast<void>(unrelated);

  Transform childT{};
  childT.parentId = world->persistent_id(parent);
  static_cast<void>(world->add_transform(child, childT));
  Transform grandT{};
  grandT.parentId = world->persistent_id(child);
  static_cast<void>(world->add_transform(grandchild, grandT));

  Entity visited[4] = {};
  std::size_t visitedCount = 0U;
  world->for_each_subtree_member(
      parent, [&visited, &visitedCount](Entity entity) noexcept {
        if (visitedCount < 4U) {
          visited[visitedCount] = entity;
        }
        ++visitedCount;
      });

  if (visitedCount != 3U) {
    std::fprintf(stderr, "FAIL: expected 3 subtree members, got %zu\n",
                 visitedCount);
    return false;
  }
  if (visited[2] != parent) {
    std::fprintf(stderr, "FAIL: root must be visited last\n");
    return false;
  }
  const bool childrenFirst =
      ((visited[0] == child) && (visited[1] == grandchild)) ||
      ((visited[0] == grandchild) && (visited[1] == child));
  if (!childrenFirst) {
    std::fprintf(stderr, "FAIL: descendants must be visited before root\n");
    return false;
  }

  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  struct TestCase {
    const char *name;
    bool (*fn)();
  };

  const TestCase tests[] = {
      {"lifecycle_begin_play", test_lifecycle_begin_play},
      {"lifecycle_tick", test_lifecycle_tick},
      {"lifecycle_end_play", test_lifecycle_end_play},
      {"full_lifecycle_sequence", test_full_lifecycle_sequence},
      {"deferred_destroy_survives_end_frame",
       test_deferred_destroy_survives_end_frame},
      {"subtree_member_iteration", test_subtree_member_iteration},
  };

  int failures = 0;
  for (const auto &tc : tests) {
    std::printf("  %-40s ", tc.name);
    if (tc.fn()) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  return (failures == 0) ? 0 : 1;
}
