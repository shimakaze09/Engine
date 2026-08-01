// Verifies the world-transform history behind fixed-step render
// interpolation: begin_update_phase snapshots the composed world TRS
// exactly, entries invalidate on clear and re-snapshot, and entities
// without a current-epoch snapshot report none.

#include <cstdio>
#include <memory>
#include <new>

#include "engine/runtime/world.h"

namespace {

using engine::math::Quat;
using engine::math::Vec3;
using engine::runtime::Transform;
using engine::runtime::World;

/// Runs one propagation pass so composed world transforms are current.
void propagate(World &world) {
  world.begin_render_prep_phase();
  world.end_frame_phase();
}

/// EXPECTATION: after moving an entity from P1 to P2 across one snapshot,
/// the history returns exactly P1 (position, rotation, and scale) while
/// the composed world transform holds P2.
int check_snapshot_captures_previous_step() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const auto entity = world->create_scene_object();
  Transform t{};
  t.position = Vec3(1.0F, 2.0F, 3.0F);
  t.rotation = Quat(0.0F, 0.70710678F, 0.0F, 0.70710678F);
  t.scale = Vec3(2.0F, 2.0F, 2.0F);
  if (!world->add_transform(entity, t)) {
    std::puts("add_transform P1 failed");
    return 1;
  }
  propagate(*world);

  world->begin_update_phase();
  world->end_frame_phase();

  t.position = Vec3(4.0F, 5.0F, 6.0F);
  if (!world->add_transform(entity, t)) {
    std::puts("add_transform P2 failed");
    return 1;
  }
  propagate(*world);

  Vec3 previousPosition{};
  Quat previousRotation{};
  Vec3 previousScale{};
  if (!world->get_previous_world_trs(entity, &previousPosition,
                                     &previousRotation, &previousScale)) {
    std::puts("previous TRS missing after snapshot");
    return 1;
  }
  if ((previousPosition.x != 1.0F) || (previousPosition.y != 2.0F) ||
      (previousPosition.z != 3.0F)) {
    std::puts("previous position mismatch");
    return 1;
  }
  if ((previousRotation.x != 0.0F) || (previousRotation.y != 0.70710678F) ||
      (previousRotation.z != 0.0F) || (previousRotation.w != 0.70710678F)) {
    std::puts("previous rotation mismatch");
    return 1;
  }
  if ((previousScale.x != 2.0F) || (previousScale.y != 2.0F) ||
      (previousScale.z != 2.0F)) {
    std::puts("previous scale mismatch");
    return 1;
  }

  const engine::runtime::WorldTransform *current =
      world->get_world_transform_read_ptr(entity);
  if ((current == nullptr) || (current->position.x != 4.0F)) {
    std::puts("current world transform mismatch");
    return 1;
  }
  return 0;
}

/// EXPECTATION: before any snapshot the history reports none; a clear
/// invalidates existing entries; the next snapshot revalidates with the
/// newer pose.
int check_history_validity_lifecycle() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const auto entity = world->create_scene_object();
  Transform t{};
  t.position = Vec3(1.0F, 0.0F, 0.0F);
  if (!world->add_transform(entity, t)) {
    return 1;
  }
  propagate(*world);

  Vec3 position{};
  Quat rotation{};
  Vec3 scale{};
  if (world->get_previous_world_trs(entity, &position, &rotation, &scale)) {
    std::puts("history valid before any snapshot");
    return 1;
  }

  world->begin_update_phase();
  world->end_frame_phase();
  if (!world->get_previous_world_trs(entity, &position, &rotation, &scale)) {
    std::puts("history missing after snapshot");
    return 1;
  }

  world->clear_world_transform_history();
  if (world->get_previous_world_trs(entity, &position, &rotation, &scale)) {
    std::puts("history survived a clear");
    return 1;
  }

  t.position = Vec3(9.0F, 0.0F, 0.0F);
  if (!world->add_transform(entity, t)) {
    return 1;
  }
  propagate(*world);
  world->begin_update_phase();
  world->end_frame_phase();
  if (!world->get_previous_world_trs(entity, &position, &rotation, &scale) ||
      (position.x != 9.0F)) {
    std::puts("re-snapshot did not capture the newer pose");
    return 1;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_snapshot_captures_previous_step();
  if (result != 0) {
    return result;
  }
  return check_history_validity_lifecycle();
}
