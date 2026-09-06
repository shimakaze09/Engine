// Verifies ecs stress 50k behavior for the Engine test suite.

#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using Clock = std::chrono::high_resolution_clock;

constexpr std::size_t kEntityCount = 50000U;
constexpr float kStepSeconds = 1.0F / 60.0F;

bool populate_world(engine::runtime::World *world) noexcept {
  if (world == nullptr) {
    return false;
  }

  for (std::size_t i = 0U; i < kEntityCount; ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      return false;
    }

    engine::runtime::Transform transform{};
    transform.position = engine::math::Vec3(static_cast<float>(i) * 0.01F, 1.0F,
                                            static_cast<float>(i) * 0.005F);
    if (!world->add_transform(entity, transform)) {
      return false;
    }

    engine::runtime::RigidBody body{};
    body.inverseMass = 1.0F;
    body.velocity = engine::math::Vec3(0.1F, 0.0F, 0.05F);
    if (!world->add_rigid_body(entity, body)) {
      return false;
    }
  }

  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    std::printf("FAIL: world allocation\n");
    return 1;
  }

  if constexpr (engine::runtime::World::kMaxEntities < kEntityCount) {
    std::printf("FAIL: world capacity %zu is below required %zu\n",
                engine::runtime::World::kMaxEntities, kEntityCount);
    return 2;
  }

  if (!populate_world(world.get())) {
    std::printf("FAIL: population at 50k entities\n");
    return 3;
  }

  if ((world->transform_count() < kEntityCount) ||
      (world->rigid_body_count() < kEntityCount)) {
    std::printf("FAIL: component counts below 50k threshold\n");
    return 4;
  }

  const auto begin = Clock::now(); // wall-clock: diagnostic
  world->begin_update_phase();
  if (!world->update_transforms_range(0U, world->transform_count(),
                                      kStepSeconds)) {
    world->end_frame_phase();
    std::printf("FAIL: update_transforms_range\n");
    return 5;
  }
  if (!engine::runtime::step_physics_range(*world, 0U, world->transform_count(),
                                           kStepSeconds)) {
    world->end_frame_phase();
    std::printf("FAIL: step_physics_range\n");
    return 6;
  }
  world->commit_update_phase();
  world->end_frame_phase();
  const auto end = Clock::now(); // wall-clock: diagnostic

  const double simMs =
      std::chrono::duration<double, std::milli>(end - begin).count();

  std::printf("[ecs_stress_50k] entities=%zu simulation_ms=%.4f\n",
              world->transform_count(), simMs);

  // Semantic gate: the step actually simulated at capacity — sampled
  // moving bodies integrated to finite, changed positions. Wall-clock
  // budgets live in the engine_bench_ suite, never in functional tests.
  const std::uint32_t sampleIndices[] = {1U, 25000U, 50000U};
  for (const std::uint32_t index : sampleIndices) {
    const engine::runtime::Entity entity = world->find_entity_by_index(index);
    const engine::runtime::Transform *transform =
        world->get_transform_read_ptr(entity);
    if (transform == nullptr) {
      std::printf("FAIL: sampled entity %u lost its transform\n", index);
      return 7;
    }
    const engine::math::Vec3 &position = transform->position;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
      std::printf("FAIL: sampled entity %u position is non-finite\n", index);
      return 8;
    }
    if ((position.x == static_cast<float>(index - 1U) * 0.01F) &&
        (position.z == static_cast<float>(index - 1U) * 0.005F)) {
      std::printf("FAIL: sampled entity %u did not integrate\n", index);
      return 9;
    }
  }

  return 0;
}
