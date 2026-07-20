// Verifies scene capture component test behavior for the Engine test suite.

#include <cmath>
#include <memory>
#include <new>

#include "engine/runtime/world.h"

namespace {

bool nearly_equal(float lhs, float rhs) noexcept {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

int verify_scene_capture_crud() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 2;
  }

  engine::runtime::SceneCaptureComponent capture{};
  capture.width = 512U;
  capture.height = 288U;
  capture.fovRadians = 0.8F;
  capture.nearPlane = 0.5F;
  capture.farPlane = 250.0F;
  capture.enabled = false;

  if (!world->add_scene_capture_component(entity, capture)) {
    return 3;
  }
  if (!world->has_scene_capture_component(entity)) {
    return 4;
  }
  if (world->scene_capture_count() != 1U) {
    return 5;
  }
  if (world->scene_capture_entity_at(0U) != entity) {
    return 6;
  }

  const engine::runtime::SceneCaptureComponent *stored =
      world->scene_capture_at(0U);
  if ((stored == nullptr) || (stored->width != 512U)) {
    return 7;
  }

  engine::runtime::SceneCaptureComponent readBack{};
  if (!world->get_scene_capture_component(entity, &readBack)) {
    return 8;
  }
  if ((readBack.width != 512U) || (readBack.height != 288U) ||
      !nearly_equal(readBack.fovRadians, 0.8F) ||
      !nearly_equal(readBack.nearPlane, 0.5F) ||
      !nearly_equal(readBack.farPlane, 250.0F) || readBack.enabled) {
    return 9;
  }

  engine::runtime::SceneCaptureComponent *mutableCapture =
      world->get_scene_capture_component_ptr(entity);
  if (mutableCapture == nullptr) {
    return 10;
  }
  mutableCapture->enabled = true;
  if (!world->get_scene_capture_component(entity, &readBack) ||
      !readBack.enabled) {
    return 11;
  }

  if (!world->remove_scene_capture_component(entity)) {
    return 12;
  }
  if (world->has_scene_capture_component(entity) ||
      (world->scene_capture_count() != 0U)) {
    return 13;
  }
  if (world->get_scene_capture_component(entity, &readBack)) {
    return 14;
  }

  return 0;
}

int verify_scene_capture_query_and_destroy() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 20;
  }

  const engine::runtime::Entity first = world->create_entity();
  const engine::runtime::Entity second = world->create_entity();
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity)) {
    return 21;
  }

  engine::runtime::SceneCaptureComponent firstCapture{};
  firstCapture.width = 128U;
  if (!world->add_scene_capture_component(first, firstCapture)) {
    return 22;
  }

  engine::runtime::SceneCaptureComponent secondCapture{};
  secondCapture.width = 64U;
  if (!world->add_scene_capture_component(second, secondCapture)) {
    return 23;
  }

  int visited = 0;
  std::uint32_t widthSum = 0U;
  world->for_each<engine::runtime::SceneCaptureComponent>(
      [&](engine::runtime::Entity,
          const engine::runtime::SceneCaptureComponent &component) noexcept {
        ++visited;
        widthSum += component.width;
      });

  if ((visited != 2) || (widthSum != 192U)) {
    return 24;
  }

  if (!world->destroy_entity(first)) {
    return 25;
  }
  if (world->has_scene_capture_component(first) ||
      (world->scene_capture_count() != 1U)) {
    return 26;
  }

  return 0;
}

int verify_scene_capture_slot_mapping() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 40;
  }

  // Dense order: enabled, disabled, enabled. Renderer slots number only the
  // enabled captures, so the third capture maps to slot 1.
  const engine::runtime::Entity first = world->create_entity();
  const engine::runtime::Entity second = world->create_entity();
  const engine::runtime::Entity third = world->create_entity();
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity) ||
      (third == engine::runtime::kInvalidEntity)) {
    return 41;
  }

  engine::runtime::SceneCaptureComponent enabledCapture{};
  enabledCapture.enabled = true;
  engine::runtime::SceneCaptureComponent disabledCapture{};
  disabledCapture.enabled = false;

  if (!world->add_scene_capture_component(first, enabledCapture) ||
      !world->add_scene_capture_component(second, disabledCapture) ||
      !world->add_scene_capture_component(third, enabledCapture)) {
    return 42;
  }

  if (world->scene_capture_slot_for_entity(first) != 0) {
    return 43;
  }
  if (world->scene_capture_slot_for_entity(second) != -1) {
    return 44;
  }
  if (world->scene_capture_slot_for_entity(third) != 1) {
    return 45;
  }

  // Entities without a capture (or stale handles) have no slot.
  const engine::runtime::Entity plain = world->create_entity();
  if (world->scene_capture_slot_for_entity(plain) != -1) {
    return 46;
  }
  if (world->scene_capture_slot_for_entity(engine::runtime::Entity{}) != -1) {
    return 47;
  }

  // Removing an enabled capture renumbers the ones after it.
  if (!world->remove_scene_capture_component(first)) {
    return 48;
  }
  if (world->scene_capture_slot_for_entity(third) != 0) {
    return 49;
  }

  return 0;
}

int verify_scene_capture_capacity() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 30;
  }

  // The fixed capacity accepts exactly kMaxSceneCaptureComponents entries.
  for (std::size_t i = 0U;
       i < engine::runtime::World::kMaxSceneCaptureComponents; ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      return 31;
    }
    if (!world->add_scene_capture_component(
            entity, engine::runtime::SceneCaptureComponent{})) {
      return 32;
    }
  }

  const engine::runtime::Entity overflow = world->create_entity();
  if (overflow == engine::runtime::kInvalidEntity) {
    return 33;
  }
  if (world->add_scene_capture_component(
          overflow, engine::runtime::SceneCaptureComponent{})) {
    return 34;
  }
  if (world->scene_capture_count() !=
      engine::runtime::World::kMaxSceneCaptureComponents) {
    return 35;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = verify_scene_capture_crud();
  if (result != 0) {
    return result;
  }

  result = verify_scene_capture_query_and_destroy();
  if (result != 0) {
    return result;
  }

  result = verify_scene_capture_slot_mapping();
  if (result != 0) {
    return result;
  }

  return verify_scene_capture_capacity();
}
