#include "engine/core/bootstrap.h"
#include "engine/core/job_system.h"
#include "engine/physics/physics.h"
#include "engine/runtime/world.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace {

constexpr std::size_t kEntityCount = 10000U;
constexpr int kUpdateSteps = 120;
constexpr float kStepSeconds = 1.0F / 60.0F;
constexpr auto kBudgetMs = 1000;
constexpr std::size_t kMinChunkSize = 64U;
constexpr std::size_t kChunkRange = 384U;
constexpr std::size_t kMaxJobs = 512U;

engine::runtime::World g_worldA;
engine::runtime::World g_worldB;

struct UpdateJobData final {
  engine::runtime::World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  float deltaSeconds = 0.0F;
};

std::uint32_t next_random(std::uint32_t *state) {
  if (state == nullptr) {
    return 0U;
  }

  *state = (*state * 1664525U) + 1013904223U;
  return *state;
}

void run_update_chunk(void *userData) noexcept {
  auto *jobData = static_cast<UpdateJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  static_cast<void>(jobData->world->update_transforms_range(
      jobData->startIndex, jobData->count, jobData->deltaSeconds));
  static_cast<void>(engine::physics::step_physics_range(
      *jobData->world, jobData->startIndex, jobData->count,
      jobData->deltaSeconds));
}

bool populate_world(engine::runtime::World *world,
                    engine::runtime::Entity *outFirstEntity) {
  if (world == nullptr) {
    return false;
  }

  bool firstSet = false;
  for (std::size_t i = 0U; i < kEntityCount; ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      return false;
    }

    if (!firstSet && (outFirstEntity != nullptr)) {
      *outFirstEntity = entity;
      firstSet = true;
    }

    engine::runtime::Transform transform{};
    transform.position = engine::math::Vec3(static_cast<float>(i), 0.0F, 0.0F);

    engine::runtime::RigidBody rigidBody{};
    rigidBody.velocity = engine::math::Vec3(1.0F, 0.0F, 0.0F);
    rigidBody.inverseMass = 1.0F;

    if (!world->add_transform(entity, transform)) {
      return false;
    }

    if (!world->add_rigid_body(entity, rigidBody)) {
      return false;
    }
  }

  return firstSet;
}

bool verify_query_iteration_contract() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }

  const engine::runtime::Entity movingEntity = world->create_entity();
  const engine::runtime::Entity staticEntity = world->create_entity();
  if ((movingEntity == engine::runtime::kInvalidEntity) ||
      (staticEntity == engine::runtime::kInvalidEntity)) {
    return false;
  }

  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  engine::runtime::RigidBody rigidBody{};
  rigidBody.velocity = engine::math::Vec3(2.0F, 0.0F, 0.0F);
  rigidBody.inverseMass = 1.0F;

  if (!world->add_transform(movingEntity, transform) ||
      !world->add_rigid_body(movingEntity, rigidBody) ||
      !world->add_transform(staticEntity, transform)) {
    return false;
  }

  std::size_t transformVelocityMatches = 0U;
  world->for_each<engine::runtime::Transform, engine::runtime::RigidBody>(
      [&transformVelocityMatches](engine::runtime::Entity,
                                  const engine::runtime::Transform &,
                                  const engine::runtime::RigidBody &) noexcept {
        ++transformVelocityMatches;
      });

  std::size_t velocityMatches = 0U;
  world->for_each<engine::runtime::RigidBody>(
      [&velocityMatches](engine::runtime::Entity,
                         const engine::runtime::RigidBody &) noexcept {
        ++velocityMatches;
      });

  return (transformVelocityMatches == 1U) && (velocityMatches == 1U);
}

bool verify_entity_generation_reuse() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  if (!world->add_transform(entity, transform)) {
    return false;
  }

  if (!world->destroy_entity(entity)) {
    return false;
  }

  if (world->is_alive(entity)) {
    return false;
  }

#ifdef NDEBUG
  engine::runtime::Transform staleTransform{};
  if (world->get_transform(entity, &staleTransform)) {
    return false;
  }
#endif

  const engine::runtime::Entity recycled = world->create_entity();
  if (recycled == engine::runtime::kInvalidEntity) {
    return false;
  }

  if (recycled.index != entity.index) {
    return false;
  }

  return recycled.generation != entity.generation;
}

bool verify_deferred_destroy_commit() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(4.0F, 0.0F, 0.0F);
  engine::runtime::RigidBody rigidBody{};
  rigidBody.velocity = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  rigidBody.inverseMass = 1.0F;

  if (!world->add_transform(entity, transform) ||
      !world->add_rigid_body(entity, rigidBody)) {
    return false;
  }

  world->begin_update_phase();
  if (!world->destroy_entity(entity)) {
    return false;
  }

  if (!world->is_alive(entity)) {
    return false;
  }

  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  if (world->is_alive(entity)) {
    return false;
  }

  const engine::runtime::Entity recycled = world->create_entity();
  if (recycled == engine::runtime::kInvalidEntity) {
    return false;
  }

  return (recycled.index == entity.index) &&
         (recycled.generation != entity.generation);
}

bool verify_transform_range_overflow_guard() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Transform transform{};
  if (!world->add_transform(entity, transform)) {
    return false;
  }

  world->begin_update_phase();
  if (!world->update_transforms_range(
          0U, std::numeric_limits<std::size_t>::max(), kStepSeconds)) {
    return false;
  }
  world->commit_update_phase();

  const engine::runtime::Entity *entities = nullptr;
  const engine::runtime::Transform *transforms = nullptr;
  world->begin_render_prep_phase();
  const bool acceptedOversizedRange = world->read_transform_range(
      0U, std::numeric_limits<std::size_t>::max(), &entities, &transforms);
  world->end_frame_phase();

  return !acceptedOversizedRange;
}

bool parallel_update(engine::runtime::World *world, float deltaSeconds,
                     std::uint32_t randomSeed) {
  if (world == nullptr) {
    return false;
  }

  if (!engine::core::begin_frame_graph()) {
    return false;
  }

  std::array<UpdateJobData, kMaxJobs> jobs{};
  std::array<std::size_t, kMaxJobs> order{};
  std::size_t jobCount = 0U;

  const std::size_t transformCount = world->transform_count();
  std::size_t cursor = 0U;
  while ((cursor < transformCount) && (jobCount < jobs.size())) {
    const std::uint32_t randomValue = next_random(&randomSeed);
    std::size_t chunkSize =
        kMinChunkSize + static_cast<std::size_t>(randomValue % kChunkRange);

    if ((cursor + chunkSize) > transformCount) {
      chunkSize = transformCount - cursor;
    }

    jobs[jobCount].world = world;
    jobs[jobCount].startIndex = cursor;
    jobs[jobCount].count = chunkSize;
    jobs[jobCount].deltaSeconds = deltaSeconds;
    order[jobCount] = jobCount;

    cursor += chunkSize;
    ++jobCount;
  }

  if (cursor != transformCount) {
    static_cast<void>(engine::core::end_frame_graph());
    return false;
  }

  for (std::size_t i = jobCount; i > 1U; --i) {
    const std::size_t swapIndex =
        static_cast<std::size_t>(next_random(&randomSeed) % i);
    const std::size_t tmp = order[i - 1U];
    order[i - 1U] = order[swapIndex];
    order[swapIndex] = tmp;
  }

  world->begin_update_phase();

  engine::core::Job commitJob{};
  commitJob.function = [](void *context) noexcept {
    auto *worldContext = static_cast<engine::runtime::World *>(context);
    if (worldContext != nullptr) {
      worldContext->commit_update_phase();
    }
  };
  commitJob.data = world;

  const engine::core::JobHandle commitHandle = engine::core::submit(commitJob);
  if (!engine::core::is_valid_handle(commitHandle)) {
    world->end_frame_phase();
    static_cast<void>(engine::core::end_frame_graph());
    return false;
  }

  for (std::size_t i = 0U; i < jobCount; ++i) {
    const std::size_t jobIndex = order[i];

    engine::core::Job job{};
    job.function = &run_update_chunk;
    job.data = &jobs[jobIndex];
    const engine::core::JobHandle updateHandle = engine::core::submit(job);
    if (!engine::core::is_valid_handle(updateHandle)) {
      world->end_frame_phase();
      static_cast<void>(engine::core::end_frame_graph());
      return false;
    }

    if (!engine::core::add_dependency(updateHandle, commitHandle)) {
      world->end_frame_phase();
      static_cast<void>(engine::core::end_frame_graph());
      return false;
    }
  }

  engine::core::wait(commitHandle);

  if (!engine::core::end_frame_graph()) {
    world->end_frame_phase();
    return false;
  }

  world->begin_render_prep_phase();
  world->end_frame_phase();
  return true;
}

std::uint64_t hash_world_state(engine::runtime::World *world) {
  if (world == nullptr) {
    return 0U;
  }

  const std::size_t transformCount = world->transform_count();
  const engine::runtime::Entity *entities = nullptr;
  const engine::runtime::Transform *transforms = nullptr;

  world->begin_render_prep_phase();
  const bool readable =
      world->read_transform_range(0U, transformCount, &entities, &transforms);
  world->end_frame_phase();
  if (!readable) {
    return 0U;
  }

  std::uint64_t hash = 1469598103934665603ULL;
  auto mix_u32 = [&hash](std::uint32_t value) {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ULL;
  };
  auto mix_float = [&mix_u32](float value) {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    mix_u32(bits);
  };
  for (std::size_t i = 0U; i < transformCount; ++i) {
    const engine::runtime::Transform &transform = transforms[i];

    mix_u32(entities[i].index);
    mix_u32(entities[i].generation);
    mix_float(transform.position.x);
    mix_float(transform.position.y);
    mix_float(transform.position.z);
    mix_float(transform.rotation.x);
    mix_float(transform.rotation.y);
    mix_float(transform.rotation.z);
    mix_float(transform.rotation.w);
    mix_float(transform.scale.x);
    mix_float(transform.scale.y);
    mix_float(transform.scale.z);
  }

  return hash;
}

} // namespace

int main() {
  if (!verify_query_iteration_contract()) {
    return 1;
  }

  if (!verify_entity_generation_reuse()) {
    return 2;
  }

  if (!verify_deferred_destroy_commit()) {
    return 3;
  }

  if (!verify_transform_range_overflow_guard()) {
    return 13;
  }

  if (!engine::core::initialize_core(1024U * 1024U)) {
    return 4;
  }

  engine::runtime::Entity firstEntityA{};
  engine::runtime::Entity firstEntityB{};
  if (!populate_world(&g_worldA, &firstEntityA) ||
      !populate_world(&g_worldB, &firstEntityB)) {
    engine::core::shutdown_core();
    return 5;
  }

  if ((g_worldA.transform_count() != kEntityCount) ||
      (g_worldB.transform_count() != kEntityCount) ||
      (g_worldA.rigid_body_count() != kEntityCount) ||
      (g_worldB.rigid_body_count() != kEntityCount)) {
    engine::core::shutdown_core();
    return 6;
  }

  const auto start = std::chrono::steady_clock::now();

  std::uint32_t seedA = 0xC0FFEE11U;
  std::uint32_t seedB = 0xA11CE55EU;

  for (int step = 0; step < kUpdateSteps; ++step) {
    if (!parallel_update(&g_worldA, kStepSeconds,
                         seedA + static_cast<std::uint32_t>(step * 17))) {
      engine::core::shutdown_core();
      return 7;
    }

    if (!parallel_update(&g_worldB, kStepSeconds,
                         seedB + static_cast<std::uint32_t>(step * 29))) {
      engine::core::shutdown_core();
      return 8;
    }
  }

  const std::uint64_t hashA = hash_world_state(&g_worldA);
  const std::uint64_t hashB = hash_world_state(&g_worldB);

  engine::runtime::Transform firstTransformA{};
  if (!g_worldA.get_transform(firstEntityA, &firstTransformA)) {
    engine::core::shutdown_core();
    return 9;
  }

  if ((hashA == 0U) || (hashB == 0U) || (hashA != hashB)) {
    engine::core::shutdown_core();
    return 10;
  }

  if (firstTransformA.position.x <= 0.0F) {
    engine::core::shutdown_core();
    return 11;
  }

  const auto end = std::chrono::steady_clock::now();
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count();
  if (elapsedMs > kBudgetMs) {
    engine::core::shutdown_core();
    return 12;
  }

  engine::core::shutdown_core();
  return 0;
}
