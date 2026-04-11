#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using Clock = std::chrono::high_resolution_clock;

constexpr std::size_t kEntityCount = 50000U;
constexpr float kStepSeconds = 1.0F / 60.0F;
constexpr double kMaxSimMs = 16.0;

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

  const auto begin = Clock::now();
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
  const auto end = Clock::now();

  const double simMs =
      std::chrono::duration<double, std::milli>(end - begin).count();

  std::printf("[ecs_stress_50k] entities=%zu simulation_ms=%.4f\n",
              world->transform_count(), simMs);

  if (simMs > kMaxSimMs) {
    std::printf("FAIL: simulation %.4fms exceeded %.2fms threshold\n", simMs,
                kMaxSimMs);
    return 7;
  }

  return 0;
}
